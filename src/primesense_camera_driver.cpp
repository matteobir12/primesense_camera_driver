#include "primesense_camera_driver.h"

// likely debug only
#include <iostream>

#include <stdexcept>
#include <cstdint>
#include <glob.h>
#include <utility>
#include <array>
#include <algorithm>

namespace PS1080 {
namespace {
constexpr std::array<std::pair<std::uint16_t, std::uint16_t>, 1> SUPPORTED_DEVS = {
    std::pair<std::uint16_t, std::uint16_t>{ 0x1d27 /* idVender */, 0x0609 /* idProduct */},
};

constexpr const char* USB_PATH = "/dev/bus/usb/**/*";

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

    const auto interfaces = FetchConnectionInterfaces(fd);
    // claim interfaces
    // todo the bAlternateSetting we're going with needs to be known here
    for (auto& iface : interfaces) {
        if (iface.bInterfaceNumber != 0 || iface.bAlternateSetting != 1)
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
void ImageFromPackets(
    const uint8_t *data, const std::size_t len, std::vector<USBIO::IscPacketResults> packet_info)
{
    std::size_t offset = 0;
    for (const auto& packet : packet_info) {
        if (packet.length != packet.actual_length)
          std::cout << packet.length << " Short packet " << packet.actual_length << std::endl;

        if (packet.status != 0)
          std::cout << "Bad packet\n";

        offset += packet.length;
    }
}

}  // namespace

void Driver::StreamRGBD()
{
    const InterfaceKey key{0, 1};
    // There should be 2, one for RGB and one for D
    const auto& ep0 = receive_endpoints_[key].second[0];
    USBIO::IsochronousConfig iso_cfg;
    iso_cfg.ep = ep0;
    iso_cfg.on_packet = ImageFromPackets;

    iso_cfg.packets_per_urb = 32;
    iso_cfg.ring_size = 8;
    receive_endpoints_[key].first.startIsochronousCapture(iso_cfg);

    const auto& ep1 = receive_endpoints_[key].second[1];
    USBIO::IsochronousConfig iso_cfg_2;
    iso_cfg_2.ep = ep1;
    iso_cfg_2.on_packet = ImageFromPackets;

    iso_cfg_2.packets_per_urb = 32;
    iso_cfg_2.ring_size = 8;
    receive_endpoints_[key].first.startIsochronousCapture(iso_cfg_2);

    while (true) {}
}

}