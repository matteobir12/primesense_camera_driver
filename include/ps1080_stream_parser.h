// Parses the PS1080 sensor data protocol that runs over the depth/image
// endpoints: a stream of mini-packets, each with a 12 byte header
// (XnSensorProtocolResponseHeader in OpenNI2), assembled into frames.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace PS1080 {

std::vector<std::uint16_t> unpack11BitDepth(const std::vector<std::uint8_t>& packed);

// Converts uncompressed YUV422 (UYVY byte order, as the sensor sends it) to
// RGB888. Returns 3 bytes per pixel, 2 pixels per 4 input bytes.
std::vector<std::uint8_t> uyvyToRGB888(const std::vector<std::uint8_t>& uyvy);

// Decompresses the PrimeSense YUV422 image compression (firmware format
// XN_IO_IMAGE_FORMAT_YUV422, the only VGA mode served over the bulk
// endpoints) into raw UYVY. line_bytes is the uncompressed line stride
// (width * 2); the codec resets its predictors at every line end.
// Ported from XnStreamUncompressYUVImagePS in OpenNI2.
std::vector<std::uint8_t> decompressPSYUV422(
    const std::vector<std::uint8_t>& compressed, std::uint16_t line_bytes);

class StreamParser {
  public:
    // Mini-packet types per stream
    static constexpr std::uint16_t DEPTH_START = 0x7100;
    static constexpr std::uint16_t DEPTH_END = 0x7500;
    static constexpr std::uint16_t IMAGE_START = 0x8100;
    static constexpr std::uint16_t IMAGE_END = 0x8500;

    // frame: raw payload bytes as sent by the device (still in the configured
    // input format, e.g. 11 bit packed). timestamp: device clock from the
    // end-of-frame packet header
    using FrameCallback =
        std::function<void(const std::vector<std::uint8_t>& frame, std::uint32_t timestamp)>;

    StreamParser(const std::uint16_t start_type, const std::uint16_t end_type,
                 FrameCallback on_frame)
        : start_type_(start_type), end_type_(end_type), on_frame_(std::move(on_frame)) {}

    // Feed a contiguous chunk of endpoint data (one iso packet's payload)
    void feed(const std::uint8_t* data, std::size_t len);

    // Call when packets were lost (iso packet with error status); the frame
    // currently being assembled is dropped
    void markPacketLost();

  private:
    enum class State { LOOKING_FOR_MAGIC, HEADER, DATA };

    static constexpr std::uint16_t FW_MAGIC = 0x4252;  // "RB" on the wire
    static constexpr std::size_t HEADER_SIZE = 12;

    void resync();

    std::uint16_t start_type_;
    std::uint16_t end_type_;
    FrameCallback on_frame_;

    State state_ = State::LOOKING_FOR_MAGIC;
    bool saw_first_magic_byte_ = false;  // magic split across two feeds

    std::uint8_t header_[HEADER_SIZE];
    std::size_t header_have_ = 0;

    std::uint16_t cur_type_ = 0;
    std::uint32_t cur_timestamp_ = 0;
    std::size_t data_remaining_ = 0;

    bool in_frame_ = false;
    bool frame_corrupt_ = false;
    bool have_last_packet_id_ = false;
    std::uint16_t last_packet_id_ = 0;
    std::vector<std::uint8_t> frame_;
};

}
