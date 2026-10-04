#pragma once
#include <stdint.h>

namespace MayapOnline {
// One atomic word closes admission and records actual owner drain ACKs.
// An idle poll is not a drain ACK: resident sockets must be closed first.
class RadioGate {
 public:
  void closeAdmission() {
    uint32_t current = __atomic_load_n(&state_, __ATOMIC_ACQUIRE);
    while (!(current & Closed)) {
      const uint32_t next = (current | Closed) & ~QuietMask;
      if (__atomic_compare_exchange_n(&state_, &current, next, false,
          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    }
  }
  bool closed() const { return (__atomic_load_n(&state_, __ATOMIC_ACQUIRE) & Closed) != 0U; }
  bool enter(uint8_t owner) {
    const uint32_t bit = 1U << owner;
    uint32_t current = __atomic_load_n(&state_, __ATOMIC_ACQUIRE);
    do {
      if (current & (Closed | bit)) return false;
    } while (!__atomic_compare_exchange_n(&state_, &current,
        (current | bit) & ~(bit << 8U), false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    return true;
  }
  void leave(uint8_t owner) { __atomic_fetch_and(&state_, ~(1U << owner), __ATOMIC_RELEASE); }
  void acknowledge(uint8_t owner) {
    const uint32_t bit = 1U << owner;
    uint32_t current = __atomic_load_n(&state_, __ATOMIC_ACQUIRE);
    do {
      if (!(current & Closed) || (current & bit)) return;
    } while (!__atomic_compare_exchange_n(&state_, &current, current | (bit << 8U),
        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
  }
  bool drained(uint8_t owners) const {
    const uint32_t current = __atomic_load_n(&state_, __ATOMIC_ACQUIRE);
    return (current & Closed) && !(current & ActiveMask) &&
        ((current >> 8U) & owners) == owners;
  }
  void release() { __atomic_fetch_and(&state_, ~(Closed | QuietMask), __ATOMIC_RELEASE); }
 private:
  static constexpr uint32_t Closed = 1UL << 31U;
  static constexpr uint32_t ActiveMask = 0x0FU, QuietMask = 0x0F00U;
  uint32_t state_ = 0U;
};
}
