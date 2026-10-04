#pragma once
#include <Arduino.h>

// MQTT/realtime handshake, Cloud HTTPS and OTA HTTPS may all need sizeable
// transient TLS working sets on this no-PSRAM ESP32-S3. Keep exactly one
// transient/bulk network allocation owner at a time, but use a budget matched
// to the operation instead of forcing a small alarm POST to meet the OTA floor.
namespace MayapNetworkIoInternal {
static uint8_t tlsBusy = 0U;
static uint32_t deferred = 0U;

constexpr uint32_t TLS_FREE_MIN_MQTT = 49152U;
constexpr uint32_t TLS_FREE_MIN_CLOUD = 65536U;
constexpr uint32_t TLS_FREE_MIN_OTA = 73728U;
constexpr uint32_t TLS_LARGEST_BLOCK_MIN = 24576U;
}

enum class MayapTlsKind : uint8_t { Mqtt, Cloud, Ota };
enum class MayapTlsDeferReason : uint8_t {
  None = 0,
  Busy,
  FreeHeap,
  LargestBlock,
};

inline bool mayapTlsBusy() {
  return __atomic_load_n(&MayapNetworkIoInternal::tlsBusy, __ATOMIC_ACQUIRE) != 0U;
}
inline uint32_t mayapTlsDeferredCount() {
  return __atomic_load_n(&MayapNetworkIoInternal::deferred, __ATOMIC_RELAXED);
}
inline uint32_t mayapTlsFreeBudget(MayapTlsKind kind) {
  using namespace MayapNetworkIoInternal;
  switch (kind) {
    case MayapTlsKind::Cloud: return TLS_FREE_MIN_CLOUD;
    case MayapTlsKind::Ota: return TLS_FREE_MIN_OTA;
    default: return TLS_FREE_MIN_MQTT;
  }
}

// Bulk realtime JSON/chunk publication shares admission with transient TLS.
// It never blocks the realtime pump or terminal ACKs, and Cloud cannot start a
// handshake in the check-then-allocate gap of a large config/history report.
class MayapNetworkBatchOperation {
 public:
  MayapNetworkBatchOperation() {
    uint8_t expected = 0U;
    acquired_ = __atomic_compare_exchange_n(&MayapNetworkIoInternal::tlsBusy,
        &expected, 2U, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    // Preserve the existing bulk-publication admission behavior; Phase A
    // changes only transient TLS budgets used by Cloud/OTA/MQTT.
    if (acquired_ && ESP.getFreeHeap() < MayapNetworkIoInternal::TLS_LARGEST_BLOCK_MIN) {
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
    uint8_t expected = 0U;
    acquired_ = __atomic_compare_exchange_n(&MayapNetworkIoInternal::tlsBusy,
        &expected, 1U, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    if (!acquired_) {
      reason_ = MayapTlsDeferReason::Busy;
      __atomic_fetch_add(&MayapNetworkIoInternal::deferred, 1U, __ATOMIC_RELAXED);
      return;
    }

    freeHeap_ = ESP.getFreeHeap();
    largestBlock_ = ESP.getMaxAllocHeap();
    if (freeHeap_ < mayapTlsFreeBudget(kind)) {
      reason_ = MayapTlsDeferReason::FreeHeap;
    } else if (largestBlock_ < MayapNetworkIoInternal::TLS_LARGEST_BLOCK_MIN) {
      reason_ = MayapTlsDeferReason::LargestBlock;
    }

    if (reason_ != MayapTlsDeferReason::None) {
      __atomic_store_n(&MayapNetworkIoInternal::tlsBusy, 0U, __ATOMIC_RELEASE);
      acquired_ = false;
      __atomic_fetch_add(&MayapNetworkIoInternal::deferred, 1U, __ATOMIC_RELAXED);
    }
  }
  ~MayapTlsOperation() {
    if (acquired_) __atomic_store_n(&MayapNetworkIoInternal::tlsBusy, 0U, __ATOMIC_RELEASE);
  }
  explicit operator bool() const { return acquired_; }
  MayapTlsDeferReason deferReason() const { return reason_; }
  uint32_t freeHeapAtAdmission() const { return freeHeap_; }
  uint32_t largestBlockAtAdmission() const { return largestBlock_; }
  MayapTlsOperation(const MayapTlsOperation &) = delete;
  MayapTlsOperation &operator=(const MayapTlsOperation &) = delete;
 private:
  bool acquired_ = false;
  MayapTlsDeferReason reason_ = MayapTlsDeferReason::None;
  uint32_t freeHeap_ = 0U;
  uint32_t largestBlock_ = 0U;
};
