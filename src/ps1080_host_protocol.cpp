#include "ps1080_host_protocol.h"

#include "usb_io/u_usbfs_io.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace PS1080 {
namespace {
// "GM" / "RB" (XN_HOST_MAGIC_26 / XN_FW_MAGIC_26)
constexpr std::uint16_t HOST_MAGIC = 0x4d47;
constexpr std::uint16_t FW_MAGIC = 0x4252;

// V26 protocol header (FW >= 1.2), all fields little endian
struct __attribute__((packed)) ProtocolHeader {
    std::uint16_t magic;
    std::uint16_t size;  // payload size in 16 bit words (header excluded)
    std::uint16_t opcode;
    std::uint16_t id;
};
static_assert(sizeof(ProtocolHeader) == 8);

constexpr int PROTOCOL_MAX_PACKET_SIZE = 512;  // FW >= 5.0
constexpr int SEND_RETRIES = 5;
constexpr int REPLY_TIMEOUT_MS = 5000;

// vendor request, everything but the direction bit is 0
constexpr std::uint8_t REQ_TYPE_OUT = 0x40;  // USB_DIR_OUT | USB_TYPE_VENDOR
constexpr std::uint8_t REQ_TYPE_IN = 0xc0;   // USB_DIR_IN | USB_TYPE_VENDOR

std::uint16_t rd16(const std::uint8_t* const p) {
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
}

}  // namespace

bool HostProtocol::execute(
    const std::uint16_t opcode,
    const std::uint16_t* const args,
    const int n_args,
    std::vector<std::uint8_t>* const reply_data,
    const bool expect_reply)
{
  std::uint8_t request[PROTOCOL_MAX_PACKET_SIZE];
  std::memset(request, 0, sizeof(request));

  const std::uint16_t id = next_id_++;
  auto* const hdr = reinterpret_cast<ProtocolHeader*>(request);
  hdr->magic = HOST_MAGIC;
  hdr->size = static_cast<std::uint16_t>(n_args);  // already in words
  hdr->opcode = opcode;
  hdr->id = id;

  for (int i = 0; i < n_args; i++)
    std::memcpy(request + sizeof(ProtocolHeader) + i * 2, &args[i], 2);

  const int request_len = sizeof(ProtocolHeader) + n_args * 2;

  int r = -1;
  for (int attempt = 0; attempt < SEND_RETRIES && r < 0; attempt++) {
    if (attempt != 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    r = USBIO::controlTransfer(fd_, REQ_TYPE_OUT, 0, 0, 0, request, request_len);
  }

  if (r < 0) {
    std::cerr << "PS1080 protocol: send of opcode " << opcode << " failed ("
              << r << ")" << std::endl;
    return false;
  }

  if (!expect_reply)
    return true;

  // poll for the reply
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(REPLY_TIMEOUT_MS);

  std::uint8_t reply[PROTOCOL_MAX_PACKET_SIZE];
  while (std::chrono::steady_clock::now() < deadline) {
    std::memset(reply, 0, sizeof(reply));
    const int got = USBIO::controlTransfer(
        fd_, REQ_TYPE_IN, 0, 0, 0, reply, sizeof(reply));

    constexpr int MIN_REPLY = sizeof(ProtocolHeader) + 2 /* error code */;
    if (got < MIN_REPLY) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }

    // the reply header may be preceded by garbage, scan for the magic
    int off = 0;
    while (off <= got - MIN_REPLY && rd16(reply + off) != FW_MAGIC)
      off++;

    if (rd16(reply + off) != FW_MAGIC) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }

    const std::uint16_t r_size = rd16(reply + off + 2);
    const std::uint16_t r_opcode = rd16(reply + off + 4);
    const std::uint16_t r_id = rd16(reply + off + 6);

    // stale reply from an earlier command, keep reading
    if (r_id != id)
      continue;

    if (r_opcode != opcode) {
      std::cerr << "PS1080 protocol: reply opcode " << r_opcode
                << " (expected " << opcode << ")" << std::endl;
      return false;
    }

    const std::uint16_t error_code = rd16(reply + off + sizeof(ProtocolHeader));
    if (error_code != 0) {
      std::cerr << "PS1080 protocol: opcode " << opcode << " NACKed with "
                << error_code << std::endl;
      return false;
    }

    if (reply_data) {
      // size counts the error code word
      const int data_len = (r_size - 1) * 2;
      const std::uint8_t* const data = reply + off + MIN_REPLY;
      const int avail = got - off - MIN_REPLY;
      reply_data->assign(data, data + std::min(data_len, avail));
    }

    return true;
  }

  std::cerr << "PS1080 protocol: timed out waiting for reply to opcode "
            << opcode << std::endl;
  return false;
}

bool HostProtocol::getVersion(FWVersion& out) {
  std::vector<std::uint8_t> data;
  if (!execute(OPCODE_GET_VERSION, nullptr, 0, &data) || data.size() < 12)
    return false;

  // first two bytes are swapped on the wire (see XnHostProtocolGetVersion)
  out.minor = data[0];
  out.major = data[1];
  out.build = rd16(&data[2]);
  out.chip = rd16(&data[4]) | (static_cast<std::uint32_t>(rd16(&data[6])) << 16);
  out.fpga = rd16(&data[8]);
  out.system = rd16(&data[10]);

  // on FW >= 5 the build is BCD: 0x0022 means build "22"
  if (out.major >= 5) {
    char bcd[8];
    std::snprintf(bcd, sizeof(bcd), "%x", out.build);
    out.build = static_cast<std::uint16_t>(std::atoi(bcd));
  }

  return true;
}

bool HostProtocol::keepAlive() {
  return execute(OPCODE_KEEP_ALIVE, nullptr, 0, nullptr);
}

bool HostProtocol::getMode(std::uint16_t& mode) {
  std::vector<std::uint8_t> data;
  if (!execute(OPCODE_GET_MODE, nullptr, 0, &data) || data.size() < 2)
      return false;

  mode = rd16(data.data());
  return true;
}

void HostProtocol::setModeNoAck(const std::uint16_t mode) {
  execute(OPCODE_SET_MODE, &mode, 1, nullptr, false /* expect_reply */);
}

bool HostProtocol::setParam(const std::uint16_t param, const std::uint16_t value) {
  const std::uint16_t args[2] = {param, value};
  return execute(OPCODE_SET_PARAM, args, 2, nullptr);
}

bool HostProtocol::getParam(const std::uint16_t param, std::uint16_t& value) {
  std::vector<std::uint8_t> data;
  if (!execute(OPCODE_GET_PARAM, &param, 1, &data) || data.size() < 2)
    return false;

  value = rd16(data.data());
  return true;
}

}
