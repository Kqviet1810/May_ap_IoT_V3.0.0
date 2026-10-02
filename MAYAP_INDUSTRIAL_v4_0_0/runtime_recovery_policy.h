#pragma once
#include <stdint.h>

namespace MayapRecovery {
enum class Service : uint8_t { Network, Mqtt, Cloud, Ota, Count };
enum class Action : uint8_t { None, Reinit, Isolate, Restart };
constexpr uint32_t SERVICE_TIMEOUT_MS[] = {30000U, 60000U, 120000U, 180000U};
constexpr uint32_t ISOLATE_AFTER_MS = 60000U;
constexpr uint32_t RESTART_AFTER_MS = 300000U;
constexpr uint32_t ISOLATE_PAUSE_MS = 30000U;
constexpr uint32_t WIFI_OFF_MS = 500U;
constexpr uint32_t WIFI_COOLDOWN_MS = 120000U;
constexpr uint32_t WIFI_OFFLINE_MS = 300000U;
constexpr uint32_t WIFI_ISOLATE_MS = 120000U;
inline uint32_t age(uint32_t now, uint32_t then) { return static_cast<uint32_t>(now - then); }
inline bool due(uint32_t now, uint32_t when) { return static_cast<int32_t>(now - when) >= 0; }

// A missing Internet connection is not a dead task. Only lack of owner progress
// enters this ladder. Admission is explicit, independent of boot level.
class ServiceWatch {
 public:
  Action update(uint32_t now, bool admitted, uint32_t beat, uint32_t ack, uint32_t timeout) {
    if (!admitted) return Action::None;
    if (ack != lastAck_) { lastAck_ = ack; failing_ = false; isolated_ = false; }
    const bool stale = age(now, beat) > timeout;
    if (!stale) return Action::None;
    if (!failing_) { failing_ = true; faultAt_ = now; return Action::Reinit; }
    if (!isolated_ && age(now, faultAt_) >= ISOLATE_AFTER_MS) {
      isolated_ = true; return Action::Isolate;
    }
    return age(now, faultAt_) >= RESTART_AFTER_MS ? Action::Restart : Action::None;
  }
 private:
  uint32_t lastAck_ = 0U;
  uint32_t faultAt_ = 0U;
  bool failing_ = false;
  bool isolated_ = false;
};

class WifiRecovery {
 public:
  void success(uint32_t now) { failures_ = 0U; outageAt_ = now; outageActive_ = false; cycles_ = 0U; }
  void failure(uint32_t now) { if (failures_ < 255U) ++failures_; offline(now); }
  void offline(uint32_t now) { if (!outageActive_) { outageAt_ = now; outageActive_ = true; } }
  bool cooldownReady(uint32_t now) const {
    return !attempted_ || age(now, lastRecoveryAt_) >= WIFI_COOLDOWN_MS;
  }
  bool wanted(uint32_t now) const {
    return (failures_ >= 6U || (outageActive_ && age(now, outageAt_) >= WIFI_OFFLINE_MS)) &&
        cooldownReady(now);
  }
  void started(uint32_t now) {
    attempted_ = true; lastRecoveryAt_ = now; failures_ = 0U;
    // Give STA a fresh retry window after radio isolation. Retaining the old
    // outage age would reset it again before an asynchronous join can finish.
    outageAt_ = now; outageActive_ = false;
    if (cycles_ < 255U) ++cycles_;
  }
  bool isolate() const { return cycles_ >= 3U; }
 private:
  uint8_t failures_ = 0U, cycles_ = 0U;
  uint32_t outageAt_ = 0U, lastRecoveryAt_ = 0U;
  bool outageActive_ = false, attempted_ = false;
};
}  // namespace MayapRecovery
