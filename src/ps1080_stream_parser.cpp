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

        if (cur_type_ == start_type_) {
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
          if (cur_type_ == end_type_) {
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

namespace {
// ITU-R BT.601 integer conversion, same coefficients as OpenNI2's YUV444ToRGB888
void yuvToRGB(const std::uint8_t y, const std::uint8_t u, const std::uint8_t v,
              std::uint8_t* const rgb) {
    const std::int32_t c = (y - 16) * 298 + 128;
    const std::int32_t d = u - 128;
    const std::int32_t e = v - 128;

    const auto clamp = [](const std::int32_t x) {
        return static_cast<std::uint8_t>(std::min(std::max(x, 0), 255));
    };

    rgb[0] = clamp((c           + 409 * e) >> 8);
    rgb[1] = clamp((c - 100 * d - 208 * e) >> 8);
    rgb[2] = clamp((c + 516 * d          ) >> 8);
}

}  // namespace

std::vector<std::uint8_t> uyvyToRGB888(const std::vector<std::uint8_t>& uyvy) {
    std::vector<std::uint8_t> rgb((uyvy.size() / 4) * 6);

    std::size_t o = 0;
    for (std::size_t i = 0; i + 3 < uyvy.size(); i += 4) {
        const std::uint8_t u = uyvy[i], y1 = uyvy[i + 1], v = uyvy[i + 2], y2 = uyvy[i + 3];
        yuvToRGB(y1, u, v, &rgb[o]);
        yuvToRGB(y2, u, v, &rgb[o + 3]);
        o += 6;
    }

    return rgb;
}

std::vector<std::uint8_t> decompressPSYUV422(
    const std::vector<std::uint8_t>& compressed, const std::uint16_t line_bytes) {
    // The stream is a sequence of 4 bit elements, one per output byte:
    //   0x0..0xc: delta of -6..+6 against the channel's last value
    //   0xd:      dummy (padding)
    //   0xf:      full value follows in the next 8 bits
    // Channels cycle U,Y1,V,Y2 with the two Y predictors linked, and all
    // predictors reset at the end of each output line.
    std::vector<std::uint8_t> out;
    out.reserve(compressed.size() * 4);

    std::uint8_t last_full[4] = {0, 0, 0, 0};
    bool high_nibble = true;
    std::uint32_t channel = 0;
    std::uint32_t cur_line_bytes = 0;

    const std::uint8_t* p = compressed.data();
    const std::uint8_t* const end = p + compressed.size();

    while (p < end) {
        const std::uint32_t c = *p;

        if (high_nibble) {
            high_nibble = false;

            if (c < 0xd0) {
                last_full[channel] += static_cast<std::int8_t>((c >> 4) - 6);
            } else if (c < 0xe0) {
                continue;  // dummy, low nibble processed next iteration
            } else {
                // full value: low nibble of this byte + high nibble of next
                std::uint32_t value = (c & 0x0f) << 4;
                if (++p == end)
                    break;

                value += (*p >> 4);
                last_full[channel] = static_cast<std::uint8_t>(value);
            }
        } else {
            const std::uint32_t nibble = c & 0x0f;
            high_nibble = true;
            p++;

            if (nibble < 0xd) {
                last_full[channel] += static_cast<std::int8_t>(nibble - 6);
            } else if (nibble < 0xe) {
                continue;  // dummy
            } else {
                if (p == end)
                    break;

                last_full[channel] = *p;  // full value is the entire next byte
                p++;
            }
        }

        out.push_back(last_full[channel]);

        channel++;
        switch (channel) {
            case 2:
                last_full[3] = last_full[1];
                break;
            case 4:
                last_full[1] = last_full[3];
                channel = 0;
                break;
        }

        if (++cur_line_bytes == line_bytes) {
            last_full[0] = last_full[1] = last_full[2] = last_full[3] = 0;
            cur_line_bytes = 0;
        }
    }

    return out;
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
