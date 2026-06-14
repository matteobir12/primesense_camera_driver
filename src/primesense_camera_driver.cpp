#include "primesense_camera_driver.h"

// likely debug only
#include <iostream>

#include <stdexcept>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <glob.h>
#include <mutex>
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

// FW >= 5.5: alt 0 carries the bulk endpoints, alt 1 the iso ones.
// Like OpenNI2 on Linux we default to BULK: the sensor's iso endpoints
// produce -EPROTO bus errors (lost data) when depth and color stream
// together, while bulk gets retransmission.
constexpr std::uint8_t DATA_INTERFACE = 0;
constexpr std::uint8_t BULK_ALT_SETTING = 0;

// FW >= 5.x endpoint layout
constexpr std::uint8_t DEPTH_EP = 0x81;
constexpr std::uint8_t IMAGE_EP = 0x82;

std::vector<USBIO::InterfaceDescriptor> FetchConnectionInterfaces(const int fd) {
    const auto dt = USBIO::getUSBDescriptorTree(fd);

    if (!dt || dt->configs.size() != 1)
        throw std::runtime_error("Unexpected when fetching desc tree");

    const auto& cfg = dt->configs[0];
    return cfg.interfaces;
}

void SaveFramePGM(const char* const path, const std::vector<std::uint16_t>& px) {
    FILE* const f = std::fopen(path, "wb");
    if (!f)
        return;

    std::fprintf(f, "P5\n%d %d\n2047\n", STREAM_WIDTH, STREAM_HEIGHT);
    for (const std::uint16_t v : px) {
        // PGM 16 bit is big endian
        const std::uint8_t be[2] = {
            static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v & 0xff)};
        std::fwrite(be, 1, 2, f);
    }
    std::fclose(f);
}

void SaveFramePPM(const char* const path, const std::vector<std::uint8_t>& rgb) {
    FILE* const f = std::fopen(path, "wb");
    if (!f)
        return;

    std::fprintf(f, "P6\n%d %d\n255\n", STREAM_WIDTH, STREAM_HEIGHT);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
}

// Frame callbacks run on the URB reaping thread; anything slow there (pixel
// conversion, disk writes) delays URB resubmission and the iso ring
// under-runs. This hands completed frames to a dedicated thread instead.
class FrameWorker {
  public:
    explicit FrameWorker(StreamParser::FrameCallback cb)
        : cb_(std::move(cb)), thread_([this] { loop(); }) {}

    // joins the processing thread; a detached thread would still be blocked
    // on cv_ when the members get destroyed (UB, hangs in glibc)
    ~FrameWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_one();
        thread_.join();
    }

    void push(const std::vector<std::uint8_t>& frame, const std::uint32_t ts) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= 4)  // backlogged; drop rather than stall
            return;

        queue_.emplace_back(frame, ts);
        cv_.notify_one();
    }

  private:
    void loop() {
        while (true) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_)
                return;

            const auto [frame, ts] = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();

            cb_(frame, ts);
        }
    }

    StreamParser::FrameCallback cb_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::pair<std::vector<std::uint8_t>, std::uint32_t>> queue_;
    bool stop_ = false;
    std::thread thread_;  // last member: starts after everything it touches
};

USBIO::BulkCaptureConfig MakeBulkConfig(
    const USBIO::EndpointDescriptor& ep, std::shared_ptr<StreamParser> parser) {
    USBIO::BulkCaptureConfig cfg;
    cfg.ep = ep;
    // parser is shared with the reading thread; it lives as long as the
    // capture does (the driver never tears streams down)
    cfg.on_data = [parser = std::move(parser)](const uint8_t* data, size_t len) {
        parser->feed(data, len);
    };

    return cfg;
}

}  // namespace

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

Driver::~Driver() {
    if (!protocol_)
        return;

    std::cout << "shutdown: stopping firmware streams" << std::endl;
    if (depth_running_)
        protocol_->setParam(HostProtocol::PARAM_GENERAL_STREAM1_MODE,
                            HostProtocol::STREAM_MODE_OFF);
    if (color_running_)
        protocol_->setParam(HostProtocol::PARAM_GENERAL_STREAM0_MODE,
                            HostProtocol::STREAM_MODE_OFF);

    std::cout << "shutdown: stopping captures" << std::endl;
    // captures (and their threads) die with receive_endpoints_
    receive_endpoints_.clear();
    std::cout << "shutdown: done" << std::endl;
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
        if (iface.bInterfaceNumber != DATA_INTERFACE || iface.bAlternateSetting != BULK_ALT_SETTING)
          continue;

        bool made_iface = false;
        for (auto& ep : iface.endpoints) {
            if (ep.transfer_type == USBIO::TransferType::BULK &&
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

void TestOnDepthFrame(const std::vector<std::uint8_t>& frame, const std::uint32_t timestamp) {
    static int frame_count = 0;
    frame_count++;

    const auto px = unpack11BitDepth(frame);

    std::uint16_t min_v = 0x7ff, max_v = 0;
    for (const std::uint16_t v : px) {
        if (v != 0 && v < min_v) min_v = v;  // 0 == no reading
        if (v > max_v) max_v = v;
    }

    const std::size_t expected = STREAM_WIDTH * STREAM_HEIGHT;
    const std::uint16_t center =
        px.size() == expected ? px[(STREAM_HEIGHT / 2) * STREAM_WIDTH + STREAM_WIDTH / 2] : 0;

    std::printf("depth frame %d: %zu bytes -> %zu px (expect %zu), ts %u, "
                "shift min/max %u/%u, center %u\n",
                frame_count, frame.size(), px.size(), expected, timestamp,
                min_v, max_v, center);

    if (frame_count % 30 == 0 && px.size() == expected) {
        SaveFramePGM("/tmp/ps1080_depth.pgm", px);
        std::printf("wrote /tmp/ps1080_depth.pgm\n");
    }
}

void TestOnColorFrame(const std::vector<std::uint8_t>& frame, const std::uint32_t timestamp) {
    static int frame_count = 0;
    frame_count++;

    const std::size_t expected = STREAM_WIDTH * STREAM_HEIGHT * 2;  // YUV422: 2 B/px
    const auto uyvy = decompressPSYUV422(frame, STREAM_WIDTH * 2);

    std::printf("color frame %d: %zu bytes -> %zu uyvy bytes (expect %zu), ts %u\n",
                frame_count, frame.size(), uyvy.size(), expected, timestamp);

    if (frame_count % 30 == 0 && uyvy.size() == expected) {
        SaveFramePPM("/tmp/ps1080_color.ppm", uyvyToRGB888(uyvy));
        std::printf("wrote /tmp/ps1080_color.ppm\n");
    }
}

void Driver::startDepthStream(StreamParser::FrameCallback cb)
{
    const InterfaceKey key{DATA_INTERFACE, BULK_ALT_SETTING};
    const auto& eps = receive_endpoints_[key].second;
    const auto ep_it = std::find_if(eps.begin(), eps.end(),
        [](const auto& ep) { return ep.bEndpointAddress == DEPTH_EP; });
    if (ep_it == eps.end())
        throw std::runtime_error("No depth bulk endpoint");

    auto worker = std::make_shared<FrameWorker>(cb);
    auto parser = std::make_shared<StreamParser>(
        StreamParser::DEPTH_START, StreamParser::DEPTH_END,
        [worker](const std::vector<std::uint8_t>& frame, const std::uint32_t ts) {
            worker->push(frame, ts);
        });

    // listen before turning the stream on so nothing is missed
    receive_endpoints_[key].first.startBulkCapture(MakeBulkConfig(*ep_it, parser));

    // configure and start the depth stream (XnSensorDepthStream in OpenNI2)
    if (!protocol_->setParam(HostProtocol::PARAM_DEPTH_FORMAT,
                             HostProtocol::DEPTH_FORMAT_UNCOMPRESSED_11_BIT) ||
        !protocol_->setParam(HostProtocol::PARAM_DEPTH_RESOLUTION,
                             HostProtocol::RESOLUTION_VGA) ||
        !protocol_->setParam(HostProtocol::PARAM_DEPTH_FPS, STREAM_FPS) ||
        !protocol_->setParam(HostProtocol::PARAM_DEPTH_HOLE_FILTER, 1))
        throw std::runtime_error("Failed to configure depth stream");

    if (!protocol_->setParam(HostProtocol::PARAM_GENERAL_STREAM1_MODE,
                             HostProtocol::STREAM_MODE_DEPTH))
        throw std::runtime_error("Failed to start depth stream");

    depth_running_ = true;
    std::cout << "Depth stream started" << std::endl;
}

void Driver::startColorStream(StreamParser::FrameCallback cb)
{
    const InterfaceKey key{DATA_INTERFACE, BULK_ALT_SETTING};
    const auto& eps = receive_endpoints_[key].second;
    const auto ep_it = std::find_if(eps.begin(), eps.end(),
        [](const auto& ep) { return ep.bEndpointAddress == IMAGE_EP; });
    if (ep_it == eps.end())
        throw std::runtime_error("No image bulk endpoint");

    auto worker = std::make_shared<FrameWorker>(cb);
    auto parser = std::make_shared<StreamParser>(
        StreamParser::IMAGE_START, StreamParser::IMAGE_END,
        [worker](const std::vector<std::uint8_t>& frame, const std::uint32_t ts) {
            worker->push(frame, ts);
        });

    receive_endpoints_[key].first.startBulkCapture(MakeBulkConfig(*ep_it, parser));

    // configure and start the color stream (XnSensorImageStream in OpenNI2).
    // The bulk endpoints only serve the PS compressed YUV422 format at VGA;
    // uncompressed YUV422 is iso only (firmware ACKs it but sends nothing).
    if (!protocol_->setParam(HostProtocol::PARAM_IMAGE_FORMAT,
                             HostProtocol::IMAGE_FORMAT_YUV422) ||
        !protocol_->setParam(HostProtocol::PARAM_IMAGE_RESOLUTION,
                             HostProtocol::RESOLUTION_VGA) ||
        !protocol_->setParam(HostProtocol::PARAM_IMAGE_FPS, STREAM_FPS))
        throw std::runtime_error("Failed to configure color stream");

    if (!protocol_->setParam(HostProtocol::PARAM_GENERAL_STREAM0_MODE,
                             HostProtocol::STREAM_MODE_COLOR))
        throw std::runtime_error("Failed to start color stream");

    color_running_ = true;
    std::cout << "Color stream started" << std::endl;
}

void Driver::StreamDepth(StreamParser::FrameCallback cb)
{
    startDepthStream(std::move(cb));

    while (!stop_requested_.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void Driver::StreamRGB(StreamParser::FrameCallback cb)
{
    startColorStream(std::move(cb));

    while (!stop_requested_.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void Driver::StreamRGBD()
{
    startColorStream();
    startDepthStream();

    while (!stop_requested_.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

}
