#pragma once
#include <stdint.h>

namespace Mayap {
// A desired-state change is not a fault while its command is in flight.
// Separate trackers cover batch and activity, so confirming one never hides
// failure of the other. No allocation, blocking wait or wire-protocol change.
class AttinyStateSync {
 public:
  static constexpr uint32_t TRANSITION_TIMEOUT_MS = 8000UL;

  void expect(bool value, uint32_t now) {
    if (!initialized_) {
      initialized_ = true;
      expected_ = value;
      return;
    }
    if (value == expected_) return; // Retries must not extend the deadline.
    expected_ = value;
    pending_ = true;
    startedAt_ = now;
    // An already established fault stays visible until actual confirmation.
  }

  void observe(bool reported, bool currentCommandComplete, uint32_t now) {
    update(now);
    // Heartbeats/old commands can carry a snapshot from before this change.
    // Even matching flags cannot settle it while an opposite command is queued.
    if (pending_ && !currentCommandComplete) return;
    pending_ = false;
    fault_ = reported != expected_; // Status bits, not mere command ACK, prove it.
  }

  // Desired state may stay unchanged while an old opposite command is still
  // active/queued (e.g. manual fan ON, then batch START before its reply).
  // Require the corrective command, without extending an existing deadline.
  void reconcile(uint32_t now) {
    if (pending_) return;
    pending_ = true;
    startedAt_ = now;
  }

  void commandFailed() {
    pending_ = false;
    fault_ = true;
  }

  void update(uint32_t now) {
    if (pending_ && static_cast<uint32_t>(now - startedAt_) >= TRANSITION_TIMEOUT_MS) {
      fault_ = true;
      // Keep requiring the current command's confirmation. A late snapshot
      // from an older command must not clear the deadline fault.
    }
  }

  bool pending() const { return pending_; }
  bool fault() const { return fault_; }

 private:
  uint32_t startedAt_ = 0U;
  bool initialized_ = false;
  bool expected_ = false;
  bool pending_ = false;
  bool fault_ = false;
};
} // namespace Mayap
