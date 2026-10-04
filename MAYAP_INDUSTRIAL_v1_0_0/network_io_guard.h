#pragma once
#include <Arduino.h>

// Transient TLS operations share one admission gate on this no-PSRAM board.
// Realtime keeps its established socket alive; only new TLS working sets are
// serialized here. Cloud alarm delivery intentionally has a lower admission
// floor than OTA because a small safety notification is time-sensitive while
// firmware update can safely wait for a larger memory reserve.
namespace MayapNetworkIoInternal {
static uint8_t tlsBusy = 0U;
static uint32_t deferred = 0U;
static uint32_t deferredBusy = 0U;
static uint32_t deferredHeap = 0U;
static uint32_t deferredLargest = 0U;

constexpr uint32_t MQTT_MIN_FREE_HEAP = 49152U;
constexpr uint32_t CLOUD_MIN_FREE_HEAP = 65536U;
constexpr uint32_t OTA_MIN_FREE_HEAP = 73728U;
constexpr uint32_t TLS_MIN_LARGEST_BLOCK = 24576U;
constexpr uint32_t BATCH_MIN_FREE_HEAP = 24576U;
}

enum class MayapTlsKind : uint8_t { Mqtt, Cloud, Ota };
enum class MayapTlsDenyReason : uint8_t { None, Busy, FreeHeap, LargestBlock };

inline bool mayapTlsBusy() {
  return __atomic_load_n(&MayapNetworkIoInternal::tlsBusy, __ATOMIC_ACQUIRE) != 0U;
}
inline uint32_t mayapTlsDeferredCount() {
  return __atomic_load_n(&MayapNetworkIoInternal::deferred, __ATOMIC_RELAXED);
}
inline uint32_t mayapTlsDeferredBusyCount() {
  return __atomic_load_n(&MayapNetworkIoInternal::deferredBusy, __ATOMIC_RELAXED);
}
inline uint32_t mayapTlsDeferredHeapCount() {
  return __atomic_load_n(&MayapNetworkIoInternal::deferredHeap, __ATOMIC_RELAXED);
}
inline uint32_t mayapTlsDeferredLargestCount() {
  return __atomic_load_n(&MayapNetworkIoInternal::deferredLargest, __ATOMIC_RELAXED);
}
inline uint32_t mayapTlsFreeBudget(MayapTlsKind kind) {
  switch (kind) {
    case MayapTlsKind::Cloud: return MayapNetworkIoInternal::CLOUD_MIN_FREE_HEAP;
    case MayapTlsKind::Ota: return MayapNetworkIoInternal::OTA_MIN_FREE_HEAP;
    default: return MayapNetworkIoInternal::MQTT_MIN_FREE_HEAP;
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
    if (acquired_ && ESP.getFreeHeap() < MayapNetworkIoInternal::BATCH_MIN_FREE_HEAP) {
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
      reason_ = MayapTlsDenyReason::Busy;
    } else if (ESP.getFreeHeap() < mayapTlsFreeBudget(kind)) {
      __atomic_store_n(&MayapNetworkIoInternal::tlsBusy, 0U, __ATOMIC_RELEASE);
      acquired_ = false;
      reason_ = MayapTlsDenyReason::FreeHeap;
    } else if (ESP.getMaxAllocHeap() < MayapNetworkIoInternal::TLS_MIN_LARGEST_BLOCK) {
      __atomic_store_n(&MayapNetworkIoInternal::tlsBusy, 0U, __ATOMIC_RELEASE);
      acquired_ = false;
      reason_ = MayapTlsDenyReason::LargestBlock;
    }

    if (!acquired_) {
      __atomic_fetch_add(&MayapNetworkIoInternal::deferred, 1U, __ATOMIC_RELAXED);
      switch (reason_) {
        case MayapTlsDenyReason::Busy:
          __atomic_fetch_add(&MayapNetworkIoInternal::deferredBusy, 1U, __ATOMIC_RELAXED);
          break;
        case MayapTlsDenyReason::FreeHeap:
          __atomic_fetch_add(&MayapNetworkIoInternal::deferredHeap, 1U, __ATOMIC_RELAXED);
          break;
        case MayapTlsDenyReason::LargestBlock:
          __atomic_fetch_add(&MayapNetworkIoInternal::deferredLargest, 1U, __ATOMIC_RELAXED);
          break;
        default: break;
      }
    }
  }
  ~MayapTlsOperation() {
    if (acquired_) __atomic_store_n(&MayapNetworkIoInternal::tlsBusy, 0U, __ATOMIC_RELEASE);
  }
  explicit operator bool() const { return acquired_; }
  MayapTlsDenyReason denyReason() const { return reason_; }
  MayapTlsOperation(const MayapTlsOperation &) = delete;
  MayapTlsOperation &operator=(const MayapTlsOperation &) = delete;
 private:
  bool acquired_ = false;
  MayapTlsDenyReason reason_ = MayapTlsDenyReason::None;
};
