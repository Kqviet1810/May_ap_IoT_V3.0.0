#pragma once
#include <Arduino.h>

// MQTT handshake, Cloud HTTPS and OTA HTTPS formerly allocated TLS working
// sets concurrently on the no-PSRAM board. Keep one transient TLS operation
// at a time; the established MQTT connection continues pumping independently.
namespace MayapNetworkIoInternal {
static uint8_t tlsBusy = 0U;
static uint32_t deferred = 0U;
static uint32_t cloudYieldUntil = 0U, cloudYieldRetryAt = 0U;
static uint8_t cloudActive = 0U;
}
// Cloud requests a bounded RAM handoff; only the realtime owner closes its
// socket. A failed Cloud task cannot keep realtime paused indefinitely.
inline bool mayapRequestCloudTlsYield(uint32_t now) {
  using namespace MayapNetworkIoInternal;
  const uint32_t retry = __atomic_load_n(&cloudYieldRetryAt, __ATOMIC_ACQUIRE);
  if (retry != 0U && static_cast<int32_t>(now - retry) < 0) return false;
  __atomic_store_n(&cloudYieldUntil, now + 15000U, __ATOMIC_RELEASE);
  __atomic_store_n(&cloudYieldRetryAt, now + 60000U, __ATOMIC_RELEASE);
  return true;
}
inline bool mayapCloudTlsYieldRequested(uint32_t now) {
  using namespace MayapNetworkIoInternal;
  const uint32_t until = __atomic_load_n(&cloudYieldUntil, __ATOMIC_ACQUIRE);
  return until != 0U && (static_cast<int32_t>(now - until) < 0 ||
      __atomic_load_n(&cloudActive, __ATOMIC_ACQUIRE) != 0U);
}
inline void mayapReleaseCloudTlsYield() {
  __atomic_store_n(&MayapNetworkIoInternal::cloudYieldUntil, 0U, __ATOMIC_RELEASE);
}
enum class MayapTlsKind : uint8_t { Mqtt, Cloud, Ota };
inline bool mayapTlsBusy() {
  return __atomic_load_n(&MayapNetworkIoInternal::tlsBusy, __ATOMIC_ACQUIRE) != 0U;
}
inline uint32_t mayapTlsDeferredCount() {
  return __atomic_load_n(&MayapNetworkIoInternal::deferred, __ATOMIC_RELAXED);
}
// Bulk MQTT JSON/chunk publication shares admission with transient TLS.
// It never blocks the MQTT pump or terminal ACKs, and Cloud cannot start a
// handshake in the check-then-allocate gap of a large config/history report.
class MayapNetworkBatchOperation {
 public:
  MayapNetworkBatchOperation() {
    uint8_t expected = 0U;
    acquired_ = __atomic_compare_exchange_n(&MayapNetworkIoInternal::tlsBusy,
        &expected, 2U, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    if (acquired_ && ESP.getFreeHeap() < 24576U) {
      __atomic_store_n(&MayapNetworkIoInternal::tlsBusy, 0U, __ATOMIC_RELEASE);
      acquired_ = false;
    }
  }
  ~MayapNetworkBatchOperation() {
    if (acquired_) __atomic_store_n(&MayapNetworkIoInternal::tlsBusy, 0U, __ATOMIC_RELEASE);
  }
  explicit operator bool() const { return acquired_; }
  MayapNetworkBatchOperation(const MayapNetworkBatchOperation &) = delete;
  MayapNetworkBatchOperation &operator=(const MayapNetworkBatchOperation &) = delete;
 private:
  bool acquired_ = false;
};
class MayapTlsOperation {
 public:
  explicit MayapTlsOperation(MayapTlsKind kind = MayapTlsKind::Mqtt) {
    kind_ = kind;
    uint8_t expected = 0U;
    acquired_ = __atomic_compare_exchange_n(&MayapNetworkIoInternal::tlsBusy,
        &expected, 1U, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    // Cloud/OTA coexist with the resident MQTT TLS connection. The old
    // 32 KiB admission was below even one TLS working set on this N8 board.
    const uint32_t freeBudget = kind == MayapTlsKind::Mqtt ? 49152U : 73728U;
    if (acquired_ && (ESP.getFreeHeap() < freeBudget || ESP.getMaxAllocHeap() < 24576U)) {
      __atomic_store_n(&MayapNetworkIoInternal::tlsBusy, 0U, __ATOMIC_RELEASE);
      acquired_ = false;
      if (kind == MayapTlsKind::Cloud) mayapRequestCloudTlsYield(millis());
    }
    if (acquired_ && kind == MayapTlsKind::Cloud)
      __atomic_store_n(&MayapNetworkIoInternal::cloudActive, 1U, __ATOMIC_RELEASE);
    if (!acquired_) __atomic_fetch_add(&MayapNetworkIoInternal::deferred, 1U, __ATOMIC_RELAXED);
  }
  ~MayapTlsOperation() {
    if (acquired_ && kind_ == MayapTlsKind::Cloud) {
      // Keep the bounded lease between registration and queued alarms;
      // otherwise WS reconnect wins the required HTTP send gap every time.
      __atomic_store_n(&MayapNetworkIoInternal::cloudActive, 0U, __ATOMIC_RELEASE);
    }
    if (acquired_) __atomic_store_n(&MayapNetworkIoInternal::tlsBusy, 0U, __ATOMIC_RELEASE);
  }
  explicit operator bool() const { return acquired_; }
  MayapTlsOperation(const MayapTlsOperation &) = delete;
  MayapTlsOperation &operator=(const MayapTlsOperation &) = delete;
 private:
  bool acquired_ = false;
  MayapTlsKind kind_ = MayapTlsKind::Mqtt;
};
