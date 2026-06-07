#include "primesense_camera_driver.h"

#include "ps1080_stream_parser.h"

// likely debug only
#include <iostream>

#include <stdexcept>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <glob.h>
#include <thread>
#include <utility>
#include <array>
#include <algorithm>

namespace PS1080 {
namespace {
constexpr std::array<std::pair<std::uint16_t, std::uint16_t>, 1> SUPPORTED_DEVS = {
    std::pair<std::uint16_t, std::uint16_t>{ 0x1d27 /* idVender */, 0x0609 /* idProduct */},
};

constexpr const char* USB_PATH = "/dev/bus/usb/**/*";

// FW >= 5.5: alt 0 carries the bulk endpoints, alt 1 the iso ones
constexpr std::uint8_t DATA_INTERFACE = 0;
constexpr std::uint8_t ISO_ALT_SETTING = 1;

constexpr std::uint8_t DEPTH_EP = 0x81;  // FW >= 5.x; image is 0x82

constexpr std::uint16_t DEPTH_WIDTH = 640;
constexpr std::uint16_t DEPTH_HEIGHT = 480;
constexpr std::uint16_t DEPTH_FPS = 30;

std::vector<USBIO::InterfaceDescriptor> FetchConnectionInterfaces(const int fd) {
    const auto dt = USBIO::getUSBDescriptorTree(fd);

    if (!dt || dt->configs.size() != 1)
        throw std::runtime_error("Unexpected when fetching desc tree");

    const auto& cfg = dt->configs[0];
    return cfg.interfaces;
}

}

Driver::Driver() {
    glob_t usb_devs_glob;
    glob(USB_PATH, 0, nullptr, &usb_devs_glob);

    for (int dev_idx = 0; dev_idx < usb_devs_glob.gl_pathc; dev_idx++) {
        char* const usb_dev_path = usb_devs_glob.gl_pathv[dev_idx];
        const auto dec = USBIO::probeUSBDeviceDescriptor(usb_dev_path);
        if (dec) {
            const auto found_dev_it = std::find_if(
                    SUPPORTED_DEVS.begin(),
                    SUPPORTED_DEVS.end(),
                    [&dec](const auto& dev_ids) {
                        return dev_ids.first == dec->idVender && dev_ids.second == dec->idProduct; });

            if (found_dev_it != SUPPORTED_DEVS.end()) {
                fd = USBIO::openUSBDEV(usb_dev_path);
                std::cout << "Found a compatible USB device" << std::endl;
                break;
            }
        }
    }

    globfree(&usb_devs_glob);

    init();
}

Driver::Driver(const char* const handle) {
    fd = USBIO::openUSBDEV(handle);
    init();
}

void Driver::init() {
    if (fd < 0)
        throw std::runtime_error("Couldn't open USB dev");

    auto dev_desc = USBIO::probeUSBDeviceDescriptor(fd);
    if (!dev_desc ||
        dev_desc->bcdUSB != 0x200 /*USB 2.0.0*/ ||
        dev_desc->bNumConfigurations != 1 /* Expect 1 config */)
        throw std::runtime_error("Bad data from USB dev");

    // talk to the firmware before touching interfaces; the handshake runs on
    // EP0 and the soft reset would clobber an already configured alt setting
    initFirmware();

    const auto interfaces = FetchConnectionInterfaces(fd);
    // claim interfaces
    for (auto& iface : interfaces) {
        if (iface.bInterfaceNumber != DATA_INTERFACE || iface.bAlternateSetting != ISO_ALT_SETTING)
          continue;

        bool made_iface = false;
        for (auto& ep : iface.endpoints) {
            if (ep.transfer_type == USBIO::TransferType::ISOCHRONOUS &&
                    (ep.bEndpointAddress & 0x80) != 0) {
                if (!made_iface) {
                    receive_endpoints_.emplace(
                        InterfaceKey{iface.bInterfaceNumber, iface.bAlternateSetting},
                        std::make_pair(USBIO::InterfaceForIso{
                            fd, iface}, std::vector<USBIO::EndpointDescriptor>{}));
                    made_iface = true;
                }

                receive_endpoints_[InterfaceKey{iface.bInterfaceNumber, iface.bAlternateSetting}]
                    .second.push_back(ep);
            }
        }
    }

    if (receive_endpoints_.size() < 1)
      throw std::runtime_error("No receive interfaces");
}

// Mirrors XnSensorFirmware::Init in OpenNI2
void Driver::initFirmware() {
    protocol_ = std::make_unique<HostProtocol>(fd);

    FWVersion ver;
    if (!protocol_->getVersion(ver))
        throw std::runtime_error("PS1080 GET_VERSION failed");

    std::cout << "PS1080 firmware " << static_cast<int>(ver.major) << "."
              << static_cast<int>(ver.minor) << "." << ver.build
              << " (chip 0x" << std::hex << ver.chip << std::dec << ")" << std::endl;

    if (ver.major != 5)
        throw std::runtime_error("Only the 5.x firmware protocol is implemented");

    std::uint16_t mode = 0;
    if (!protocol_->getMode(mode))
        throw std::runtime_error("PS1080 GET_MODE failed");
    if (mode == HostProtocol::MODE_SAFE_MODE)
        throw std::runtime_error("Device is in safe mode, cannot stream");

    bool alive = false;
    for (int i = 0; i < 5 && !alive; i++)
        alive = protocol_->keepAlive();
    if (!alive)
        throw std::runtime_error("PS1080 keep alive failed");

    // soft reset so the firmware starts from a clean state
    protocol_->setModeNoAck(HostProtocol::MODE_SOFT_RESET);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    alive = false;
    for (int i = 0; i < 10 && !alive; i++) {
        alive = protocol_->keepAlive();
        if (!alive)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!alive)
        throw std::runtime_error("PS1080 did not come back from soft reset");

    if (!protocol_->getMode(mode) || mode == HostProtocol::MODE_SAFE_MODE)
        throw std::runtime_error("Bad device mode after soft reset");

    std::cout << "PS1080 firmware initialized (mode " << mode << ")" << std::endl;
}

std::string Driver::fetchStringFromST(const int indx) {
    USBIO::Transfer t_for_string;
    t_for_string.usb_fd = fd;
    t_for_string.type = USBIO::TransferType::CONTROL;

    char s_buffer[255];
    USBIO::transferForUSBString(&t_for_string, indx, s_buffer, sizeof(s_buffer));
    const auto r = USBIO::transfer(&t_for_string);
    if (r != USBIO::TransferError::SUCCESS)
        return "ERROR";

    // returned buffer is (char) data len, (char) desc (doesn't matter), (UCS-2) data
    const std::uint8_t data_len = s_buffer[0] - 2;

    // simple USC-2 to ASCII
    char ascii_str[data_len];
    for (std::uint8_t i = 0; i < data_len; i++)
        ascii_str[i] = s_buffer[(i * 2) + 2];

    return std::string(ascii_str);
}

namespace {
void SaveFramePGM(const char* const path, const std::vector<std::uint16_t>& px) {
    FILE* const f = std::fopen(path, "wb");
    if (!f)
        return;

    std::fprintf(f, "P5\n%d %d\n2047\n", DEPTH_WIDTH, DEPTH_HEIGHT);
    for (const std::uint16_t v : px) {
        // PGM 16 bit is big endian
        const std::uint8_t be[2] = {
            static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v & 0xff)};
        std::fwrite(be, 1, 2, f);
    }
    std::fclose(f);
}

void OnDepthFrame(const std::vector<std::uint8_t>& frame, const std::uint32_t timestamp) {
    static int frame_count = 0;
    frame_count++;

    const auto px = unpack11BitDepth(frame);

    std::uint16_t min_v = 0x7ff, max_v = 0;
    for (const std::uint16_t v : px) {
        if (v != 0 && v < min_v) min_v = v;  // 0 == no reading
        if (v > max_v) max_v = v;
    }

    const std::size_t expected = DEPTH_WIDTH * DEPTH_HEIGHT;
    const std::uint16_t center =
        px.size() == expected ? px[(DEPTH_HEIGHT / 2) * DEPTH_WIDTH + DEPTH_WIDTH / 2] : 0;

    std::printf("depth frame %d: %zu bytes -> %zu px (expect %zu), ts %u, "
                "shift min/max %u/%u, center %u\n",
                frame_count, frame.size(), px.size(), expected, timestamp,
                min_v, max_v, center);

    if (frame_count % 30 == 0 && px.size() == expected) {
        SaveFramePGM("/tmp/ps1080_depth.pgm", px);
        std::printf("wrote /tmp/ps1080_depth.pgm\n");
    }
}

}  // namespace

void Driver::StreamDepth()
{
    const InterfaceKey key{DATA_INTERFACE, ISO_ALT_SETTING};
    const auto& eps = receive_endpoints_[key].second;
    const auto depth_ep_it = std::find_if(eps.begin(), eps.end(),
        [](const auto& ep) { return ep.bEndpointAddress == DEPTH_EP; });
    if (depth_ep_it == eps.end())
        throw std::runtime_error("No depth iso endpoint");

    // parser is shared with the iso reaping thread; it lives as long as
    // this function never returns
    auto parser = std::make_shared<StreamParser>(OnDepthFrame);

    USBIO::IsochronousConfig iso_cfg;
    iso_cfg.ep = *depth_ep_it;
    iso_cfg.packets_per_urb = 32;
    iso_cfg.ring_size = 8;
    iso_cfg.on_packet = [parser](
        const uint8_t* data, size_t /*len*/, std::vector<USBIO::IscPacketResults> packets) {
        std::size_t offset = 0;
        for (const auto& packet : packets) {
            if (packet.status != 0)
                parser->markPacketLost();
            else if (packet.actual_length > 0)
                parser->feed(data + offset, packet.actual_length);

            offset += packet.length;
        }
    };

    // listen before turning the stream on so nothing is missed
    receive_endpoints_[key].first.startIsochronousCapture(iso_cfg);

    // configure and start the depth stream (XnSensorDepthStream in OpenNI2)
    if (!protocol_->setParam(HostProtocol::PARAM_DEPTH_FORMAT,
                             HostProtocol::DEPTH_FORMAT_UNCOMPRESSED_11_BIT) ||
        !protocol_->setParam(HostProtocol::PARAM_DEPTH_RESOLUTION,
                             HostProtocol::RESOLUTION_VGA) ||
        !protocol_->setParam(HostProtocol::PARAM_DEPTH_FPS, DEPTH_FPS) ||
        !protocol_->setParam(HostProtocol::PARAM_DEPTH_HOLE_FILTER, 1))
        throw std::runtime_error("Failed to configure depth stream");

    if (!protocol_->setParam(HostProtocol::PARAM_GENERAL_STREAM1_MODE,
                             HostProtocol::STREAM_MODE_DEPTH))
        throw std::runtime_error("Failed to start depth stream");

    std::cout << "Depth stream started" << std::endl;

    while (true)
        std::this_thread::sleep_for(std::chrono::seconds(1));
}

}
