#pragma once
#include <stdint.h>

namespace Mayap {
// One authenticated upload per physical boot; lifetime is uptime, not Wi-Fi
// connected time. Recovery/reconnect must never renew the window.
class ArduinoOtaWindow {
 public:
  static constexpr uint32_t WINDOW_MS = 30UL * 60UL * 1000UL;
  void begin(bool physicalReset, uint32_t now) {
    if (initialized_) return;
    initialized_ = true;
    closed_ = !physicalReset || now >= WINDOW_MS;
  }
  bool available(uint32_t now) {
    if (now >= WINDOW_MS) closed_ = true; // Latch before millis rollover.
    return initialized_ && !closed_;
  }
  bool consume(uint32_t now) {
    if (!available(now)) return false;
    closed_ = true;
    return true;
  }
 private:
  bool initialized_ = false;
  bool closed_ = true;
};
} // namespace Mayap
