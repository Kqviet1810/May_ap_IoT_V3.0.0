#pragma once
#include <stdint.h>

namespace MayapNotes {
constexpr uint32_t RECONCILE_INTERVAL_MS = 2000U;
constexpr uint8_t RECONCILE_MAX_ATTEMPTS = 4U;
constexpr uint32_t RECONCILE_MAX_WINDOW_MS = 12000U;

class ReconcileBudget {
 public:
  void reset() { active_ = false; startedAt_ = 0U; attempts_ = 0U; }
  void begin(uint32_t now) { active_ = true; startedAt_ = now; attempts_ = 0U; }
  bool active() const { return active_; }
  uint8_t attempts() const { return attempts_; }
  bool due(uint32_t now, uint32_t lastAttemptAt) const {
    return active_ && static_cast<uint32_t>(now - lastAttemptAt) >= RECONCILE_INTERVAL_MS;
  }
  void markAttempt() { if (attempts_ < 0xFFU) ++attempts_; }
  bool exhausted(uint32_t now) const {
    return active_ && (attempts_ >= RECONCILE_MAX_ATTEMPTS ||
      static_cast<uint32_t>(now - startedAt_) >= RECONCILE_MAX_WINDOW_MS);
  }

 private:
  uint32_t startedAt_ = 0U;
  uint8_t attempts_ = 0U;
  bool active_ = false;
};

static_assert(RECONCILE_MAX_ATTEMPTS > 0U, "Notes reconcile must be finite but non-zero");
static_assert(RECONCILE_MAX_WINDOW_MS > RECONCILE_INTERVAL_MS,
              "Notes reconcile window must allow at least one retry");
} // namespace MayapNotes
