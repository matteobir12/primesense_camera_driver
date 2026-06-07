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

class StreamParser {
  public:
    // Mini-packet types for the depth stream
    static constexpr std::uint16_t DEPTH_START = 0x7100;
    static constexpr std::uint16_t DEPTH_BUFFER = 0x7200;
    static constexpr std::uint16_t DEPTH_END = 0x7500;

    // frame: raw payload bytes as sent by the device (still in the configured
    // input format, e.g. 11 bit packed). timestamp: device clock from the
    // end-of-frame packet header
    using FrameCallback =
        std::function<void(const std::vector<std::uint8_t>& frame, std::uint32_t timestamp)>;

    explicit StreamParser(FrameCallback on_frame) : on_frame_(std::move(on_frame)) {}

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
