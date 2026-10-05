#pragma once
#include <stdint.h>

namespace MayapNetwork {
// Radio-owner only. Raw admission is immediate; this policy is presentation only.
class StableWifiState {
 public:
  static constexpr uint32_t DOWN_MS = 4000U;
  static constexpr uint32_t UP_MS = 750U;
  bool update(uint32_t now, bool raw) {
    if (raw == connected_) { pending_ = false; return connected_; }
    if (!pending_ || candidate_ != raw) {
      pending_ = true; candidate_ = raw; since_ = now; samples_ = 1U;
    } else if (samples_ < 255U) { ++samples_; }
    if (static_cast<uint32_t>(now - since_) >= (raw ? UP_MS : DOWN_MS) &&
        (!raw || samples_ >= 3U)) {
      connected_ = raw; pending_ = false;
    }
    return connected_;
  }
  void reset() { connected_ = false; pending_ = false; }
 private:
  bool connected_ = false, pending_ = false, candidate_ = false;
  uint32_t since_ = 0U;
  uint8_t samples_ = 0U;
};
}  // namespace MayapNetwork
