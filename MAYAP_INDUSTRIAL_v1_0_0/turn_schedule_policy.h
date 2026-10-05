#pragma once
#include <stdint.h>

namespace MayapTurning {

// Pure scheduler projection used by production and host regression.
// The persistent anchor is the epoch of the last REAL schedule-relevant turn.
// Homing never changes this anchor.
struct EpochSchedule {
  bool scheduled = false;
  bool due = false;
  uint32_t remainingMs = 0U;
};

inline EpochSchedule fromEpoch(uint32_t currentEpoch,
                               uint32_t lastTurnEpoch,
                               uint16_t intervalMin) {
  EpochSchedule result{};
  if (currentEpoch == 0U || lastTurnEpoch == 0U || intervalMin == 0U) {
    return result;
  }

  const uint64_t intervalSec = static_cast<uint64_t>(intervalMin) * 60ULL;
  const uint64_t dueEpoch = static_cast<uint64_t>(lastTurnEpoch) + intervalSec;
  result.scheduled = true;
  if (dueEpoch <= currentEpoch) {
    result.due = true;
    result.remainingMs = 0U;
    return result;
  }

  const uint64_t remainingMs =
      (dueEpoch - static_cast<uint64_t>(currentEpoch)) * 1000ULL;
  result.remainingMs = static_cast<uint32_t>(
      remainingMs > 0x7FFFFFFFULL ? 0x7FFFFFFFULL : remainingMs);
  return result;
}

}  // namespace MayapTurning
