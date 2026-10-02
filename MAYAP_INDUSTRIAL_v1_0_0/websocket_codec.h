#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace MayapWebSocket {
constexpr size_t FRAME_CAP = 2048U;
inline bool utf8(const uint8_t *p, size_t n) {
  uint32_t value = 0, minimum = 0; uint8_t left = 0;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t c = p[i];
    if (left) { if ((c & 0xc0) != 0x80) return false; value = (value << 6) | (c & 63);
      if (!--left && (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))) return false; }
    else if (c < 0x80) {}
    else if (c >= 0xc2 && c <= 0xdf) { value = c & 31; left = 1; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { value = c & 15; left = 2; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { value = c & 7; left = 3; minimum = 0x10000; }
    else return false;
  }
  return !left;
}
// Incremental server-frame parser: no allocation, bounded fragmentation,
// interleaved control frames, strict RSV/opcode/mask/canonical-length checks.
class Reader {
 public:
  void reset() { headerUsed_ = 0; headerNeed_ = 2; used_ = length_ = assembled_ = 0; fragmented_ = false; }
  template<class Handler> bool push(const uint8_t *bytes, size_t n, Handler handler) {
    for (size_t i = 0; i < n; ++i) {
      if (headerUsed_ < headerNeed_) {
        header_[headerUsed_++] = bytes[i];
        if (headerUsed_ == 2) {
          opcode_ = header_[0] & 15; fin_ = (header_[0] & 0x80) != 0;
          const uint8_t shortLength = header_[1] & 127;
          if ((header_[0] & 0x70) || (header_[1] & 0x80) ||
              !(opcode_ == 0 || opcode_ == 1 || opcode_ == 8 || opcode_ == 9 || opcode_ == 10)) return false;
          if (opcode_ >= 8 && (!fin_ || shortLength > 125)) return false;
          headerNeed_ = shortLength == 126 ? 4 : shortLength == 127 ? 10 : 2;
        }
        if (headerUsed_ == headerNeed_) {
          length_ = header_[1] & 127;
          if (headerNeed_ > 2) {
            uint64_t wide = 0;
            for (uint8_t j = 2; j < headerNeed_; ++j) wide = (wide << 8) | header_[j];
            if (wide > FRAME_CAP || (headerNeed_ == 4 && wide < 126) || (headerNeed_ == 10 && wide < 65536)) return false;
            length_ = static_cast<size_t>(wide);
          }
          if (length_ > FRAME_CAP || (opcode_ == 0 && !fragmented_) || (opcode_ == 1 && fragmented_)) return false;
          if (!length_ && !complete(handler)) return false;
        }
      } else {
        frame_[used_++] = bytes[i];
        if (used_ == length_ && !complete(handler)) return false;
      }
    }
    return true;
  }
 private:
  template<class Handler> bool complete(Handler handler) {
    if (opcode_ >= 8) { if (opcode_ == 8 && length_ == 1) return false;
      if (!handler(opcode_, frame_, length_)) return false;
    } else {
      if (assembled_ + length_ > FRAME_CAP) return false;
      memcpy(message_ + assembled_, frame_, length_); assembled_ += length_;
      if (fin_) { if (!utf8(message_, assembled_) || !handler(1, message_, assembled_)) return false;
        assembled_ = 0; fragmented_ = false; }
      else fragmented_ = true;
    }
    headerUsed_ = 0; headerNeed_ = 2; used_ = length_ = 0; return true;
  }
  uint8_t header_[10] = {}, headerUsed_ = 0, headerNeed_ = 2, opcode_ = 0;
  uint8_t frame_[FRAME_CAP] = {}, message_[FRAME_CAP] = {};
  size_t used_ = 0, length_ = 0, assembled_ = 0;
  bool fragmented_ = false, fin_ = false;
};
inline size_t clientFrame(uint8_t *out, size_t capacity, uint8_t opcode,
                          const uint8_t *data, size_t length, uint32_t mask) {
  const size_t header = length < 126 ? 6 : 8;
  if (length > FRAME_CAP || capacity < header + length || (opcode >= 8 && length > 125)) return 0;
  out[0] = 0x80 | opcode;
  out[1] = 0x80 | (length < 126 ? static_cast<uint8_t>(length) : 126);
  if (length >= 126) { out[2] = length >> 8; out[3] = length & 255; }
  const size_t offset = header - 4;
  for (size_t i = 0; i < 4; ++i) out[offset + i] = mask >> (i * 8);
  for (size_t i = 0; i < length; ++i) out[header + i] = data[i] ^ out[offset + i % 4];
  return header + length;
}
} // namespace MayapWebSocket
