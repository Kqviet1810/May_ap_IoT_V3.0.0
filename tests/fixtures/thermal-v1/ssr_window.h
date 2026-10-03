// Frozen verbatim from main 9569fcc; comparisons never use the new scheduler here.
class LegacySsrWindow {
 public:
  bool ssrWindowOn(uint32_t now, float power, uint16_t cycleSec) {
    const uint32_t windowMs = std::max<uint32_t>(1000UL,
        static_cast<uint32_t>(cycleSec) * 1000UL);
    if (ssrWindowStartedAt_ == 0U) ssrWindowStartedAt_ = now;
    const uint32_t elapsed = elapsedMs(now, ssrWindowStartedAt_);
    if (elapsed >= windowMs) {
      ssrWindowStartedAt_ += (elapsed / windowMs) * windowMs;
    }
    const float clamped = clampFloat(power, 0.0f, 100.0f);
    uint32_t onMs = static_cast<uint32_t>(
        clamped * static_cast<float>(windowMs) / 100.0f);
    if (onMs < SSR_MIN_ON_MS) onMs = 0;
    else if (windowMs - onMs < SSR_MIN_OFF_MS) onMs = windowMs;
    return elapsedMs(now, ssrWindowStartedAt_) < onMs;
  }
 private:
  uint32_t ssrWindowStartedAt_ = 0;
};
