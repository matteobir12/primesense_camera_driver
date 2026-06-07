#include "ps1080_stream_parser.h"

#include <algorithm>
#include <cstring>

namespace PS1080 {
namespace {
std::uint16_t rd16le(const std::uint8_t* const p) {
  return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint16_t rd16be(const std::uint8_t* const p) {
  return static_cast<std::uint16_t>(p[1]) | (static_cast<std::uint16_t>(p[0]) << 8);
}

std::uint32_t rd32le(const std::uint8_t* const p) {
  return static_cast<std::uint32_t>(rd16le(p)) |
          (static_cast<std::uint32_t>(rd16le(p + 2)) << 16);
}

}  // namespace

void StreamParser::resync() {
  state_ = State::LOOKING_FOR_MAGIC;
  saw_first_magic_byte_ = false;
  header_have_ = 0;
}

void StreamParser::markPacketLost() {
  frame_corrupt_ = true;
  resync();
}

void StreamParser::feed(const std::uint8_t* data, std::size_t len) {
  const std::uint8_t magic_lo = FW_MAGIC & 0xff;         // 0x52 R
  const std::uint8_t magic_hi = (FW_MAGIC >> 8) & 0xff;  // 0x42 B

  while (len > 0) {
    switch (state_) {
      case State::LOOKING_FOR_MAGIC: {
        if (saw_first_magic_byte_ && data[0] == magic_hi) {
          // magic was split across the previous chunk and this one
          saw_first_magic_byte_ = false;
          header_[0] = magic_lo;
          header_[1] = magic_hi;
          header_have_ = 2;
          state_ = State::HEADER;
          data++;
          len--;
          break;
        }

        saw_first_magic_byte_ = false;
        std::size_t i = 0;
        while (i + 1 < len && !(data[i] == magic_lo && data[i + 1] == magic_hi))
          i++;

        if (i + 1 < len) {
          header_[0] = magic_lo;
          header_[1] = magic_hi;
          header_have_ = 2;
          state_ = State::HEADER;
          data += i + 2;
          len -= i + 2;
        } else {
          saw_first_magic_byte_ = (len > 0 && data[len - 1] == magic_lo);
          len = 0;
        }
        break;
      }

      case State::HEADER: {
        const std::size_t take = std::min(len, HEADER_SIZE - header_have_);
        std::memcpy(header_ + header_have_, data, take);
        header_have_ += take;
        data += take;
        len -= take;

        if (header_have_ < HEADER_SIZE)
          break;

        cur_type_ = rd16le(header_ + 2);
        const std::uint16_t packet_id = rd16le(header_ + 4);
        // buf size is the only big endian field
        const std::uint16_t buf_size = rd16be(header_ + 6);
        cur_timestamp_ = rd32le(header_ + 8);

        if (buf_size < HEADER_SIZE) {
          frame_corrupt_ = true;
          resync();
          break;
        }

        if (have_last_packet_id_ &&
            packet_id != static_cast<std::uint16_t>(last_packet_id_ + 1))
          frame_corrupt_ = true;

        last_packet_id_ = packet_id;
        have_last_packet_id_ = true;

        if (cur_type_ == DEPTH_START) {
          frame_.clear();
          in_frame_ = true;
          frame_corrupt_ = false;
        }

        data_remaining_ = buf_size - HEADER_SIZE;
        state_ = State::DATA;
        break;
      }

      case State::DATA: {
        const std::size_t take = std::min(len, data_remaining_);

        if (in_frame_ && !frame_corrupt_)
          frame_.insert(frame_.end(), data, data + take);

        data += take;
        len -= take;
        data_remaining_ -= take;

        if (data_remaining_ == 0) {
          if (cur_type_ == DEPTH_END) {
            if (in_frame_ && !frame_corrupt_ && on_frame_)
                on_frame_(frame_, cur_timestamp_);
            in_frame_ = false;
          }
          resync();
        }
        break;
      }
    }
  }
}

std::vector<std::uint16_t> unpack11BitDepth(const std::vector<std::uint8_t>& packed) {
  std::vector<std::uint16_t> out;
  out.reserve((packed.size() * 8) / 11);

  std::uint32_t acc = 0;
  int bits = 0;
  for (const std::uint8_t byte : packed) {
    acc = (acc << 8) | byte;
    bits += 8;
    while (bits >= 11) {
      out.push_back(static_cast<std::uint16_t>((acc >> (bits - 11)) & 0x7ff));
      bits -= 11;
    }
  }

  return out;
}

}
