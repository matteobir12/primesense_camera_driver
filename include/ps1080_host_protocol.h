// PS1080 proprietary "host protocol", spoken over vendor control transfers on EP0.
// Reverse engineered from OpenNI2 (Source/Drivers/PS1080/Sensor/XnHostProtocol.cpp)
#pragma once

#include <cstdint>
#include <vector>

namespace PS1080 {

struct FWVersion {
    std::uint8_t major = 0;
    std::uint8_t minor = 0;
    std::uint16_t build = 0;
    std::uint32_t chip = 0;
    std::uint16_t fpga = 0;
    std::uint16_t system = 0;
};

class HostProtocol {
  public:
    // Opcodes (FW >= 3.0, EPsProtocolOpCodes in OpenNI2)
    enum Opcode : std::uint16_t {
        OPCODE_GET_VERSION = 0,
        OPCODE_KEEP_ALIVE = 1,
        OPCODE_GET_PARAM = 2,
        OPCODE_SET_PARAM = 3,
        OPCODE_GET_FIXED_PARAMS = 4,
        OPCODE_GET_MODE = 5,
        OPCODE_SET_MODE = 6,
    };

    // Firmware params (XnParams.h in OpenNI2)
    enum Param : std::uint16_t {
        PARAM_GENERAL_STREAM0_MODE = 5,  // image stream
        PARAM_GENERAL_STREAM1_MODE = 6,  // depth stream
        PARAM_IMAGE_FORMAT = 12,
        PARAM_IMAGE_RESOLUTION = 13,
        PARAM_IMAGE_FPS = 14,
        PARAM_DEPTH_FORMAT = 18,
        PARAM_DEPTH_RESOLUTION = 19,
        PARAM_DEPTH_FPS = 20,
        PARAM_DEPTH_HOLE_FILTER = 22,
        PARAM_DEPTH_MIRROR = 23,
    };

    // Values for PARAM_GENERAL_STREAMx_MODE
    enum StreamMode : std::uint16_t {
        STREAM_MODE_OFF = 0,
        STREAM_MODE_COLOR = 1,
        STREAM_MODE_DEPTH = 2,
        STREAM_MODE_IR = 3,
    };

    // Values for PARAM_IMAGE_FORMAT
    enum ImageFormat : std::uint16_t {
        IMAGE_FORMAT_BAYER = 0,
        IMAGE_FORMAT_YUV422 = 1,
        IMAGE_FORMAT_JPEG = 2,
        IMAGE_FORMAT_UNCOMPRESSED_YUV422 = 5,
        IMAGE_FORMAT_UNCOMPRESSED_BAYER = 6,
    };

    // Values for PARAM_DEPTH_FORMAT
    enum DepthFormat : std::uint16_t {
        DEPTH_FORMAT_UNCOMPRESSED_16_BIT = 0,
        DEPTH_FORMAT_COMPRESSED_PS = 1,
        DEPTH_FORMAT_UNCOMPRESSED_10_BIT = 2,
        DEPTH_FORMAT_UNCOMPRESSED_11_BIT = 3,
        DEPTH_FORMAT_UNCOMPRESSED_12_BIT = 4,
    };

    enum Resolution : std::uint16_t {
        RESOLUTION_QVGA = 0,  // 320x240
        RESOLUTION_VGA = 1,   // 640x480
    };

    // Device modes (XnHostProtocolModeType). GET_MODE returns these, SET_MODE takes
    enum Mode : std::uint16_t {
        MODE_WEBCAM = 0,
        MODE_PS = 1,
        MODE_MAINTENANCE = 2,
        MODE_SOFT_RESET = 3,
        MODE_REBOOT = 4,
        MODE_SAFE_MODE = 10,
    };

    explicit HostProtocol(const int fd) : fd_(fd) {}

    bool getVersion(FWVersion& out);
    bool keepAlive();
    bool getMode(std::uint16_t& mode);
    // fire-and-forget (OpenNI2 does the same)
    void setModeNoAck(const std::uint16_t mode);
    bool setParam(const std::uint16_t param, const std::uint16_t value);
    bool getParam(const std::uint16_t param, std::uint16_t& value);

  private:
    // Sends opcode + args, waits for ACKed reply. Returns reply payload
    // (may be empty) or no value on failure.
    bool execute(const std::uint16_t opcode,
                 const std::uint16_t* const args, const int n_args,
                 std::vector<std::uint8_t>* const reply_data,
                 const bool expect_reply = true);

    int fd_ = -1;
    std::uint16_t next_id_ = 0;
};

}
