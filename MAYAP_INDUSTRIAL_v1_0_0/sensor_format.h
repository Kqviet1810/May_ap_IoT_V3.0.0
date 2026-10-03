#pragma once

// Pure raw-register decoding. The Modbus register order stays RH, temperature.
enum class SensorTemperatureFormat : uint8_t {
  Auto = 0, TempX10 = 1, TempX100 = 2, Sht30Raw16 = 3
};

class SensorFormatDecoder {
 public:
  explicit SensorFormatDecoder(SensorTemperatureFormat configured = SensorTemperatureFormat::Auto,
                               bool humidityRaw16 = false)
      : configured_(configured), humidityRaw16_(humidityRaw16) {}

  static float decodeTemperature(uint16_t raw, SensorTemperatureFormat format) {
    // X10/X100 registers use signed two's complement; native SHT30 is unsigned.
    const int32_t signedRaw = raw < 0x8000U ? raw : static_cast<int32_t>(raw) - 65536;
    switch (format) {
      case SensorTemperatureFormat::TempX10: return signedRaw * 0.1f;
      case SensorTemperatureFormat::TempX100: return signedRaw * 0.01f;
      case SensorTemperatureFormat::Sht30Raw16: return -45.0f + 175.0f * raw / 65535.0f;
      default: return NAN;
    }
  }

  bool accept(uint16_t rawTemperature, uint16_t rawHumidity) {
    valid_ = false;
    humidity_ = humidityRaw16_ ? 100.0f * rawHumidity / 65535.0f : rawHumidity * 0.1f;
    if (humidity_ < 0.0f || humidity_ > 100.0f) { rejectSample(); return false; }
    if (!locked()) {
      uint8_t candidates = 0U;
      for (uint8_t i = 1U; i <= 3U; ++i) {
        const auto f = static_cast<SensorTemperatureFormat>(i);
        if (configured_ != SensorTemperatureFormat::Auto && f != configured_) continue;
        const float t = decodeTemperature(rawTemperature, f);
        // Raw-only autodetection is ambiguous outside this commissioning envelope.
        // Explicit verified module profiles can start anywhere in the old -40..60 C range.
        const float minimum = configured_ == SensorTemperatureFormat::Auto ? 10.0f : -40.0f;
        if (t >= minimum && t <= 60.0f) candidates |= static_cast<uint8_t>(1U << i);
      }
      if (!candidates || (candidates & (candidates - 1U))) { rejectSample(); return false; }
      if (candidates != candidateMask_) { candidateMask_ = candidates; consecutive_ = 0U; }
      if (++consecutive_ < 6U) return false;
      for (uint8_t i = 1U; i <= 3U; ++i)
        if (candidates == (1U << i)) format_ = static_cast<SensorTemperatureFormat>(i);
    }
    const float t = decodeTemperature(rawTemperature, format_);
    if (!isfinite(t) || t < -40.0f || t > 60.0f ||
        (hasLast_ && lastTemperature_ - t > 20.0f)) {
      rejectSample();
      return false; // Never change scale, even through loss/recovery of UART.
    }
    // Never suppress a plausible upward raw step: EmergencyHigh must see it
    // immediately, independently of median/IIR lag or PID plausibility gates.
    temperature_ = lastTemperature_ = t;
    hasLast_ = valid_ = true;
    return true;
  }

  void rejectSample() { valid_ = false; consecutive_ = 0U; candidateMask_ = 0U; }
  bool locked() const { return format_ != SensorTemperatureFormat::Auto; }
  bool valid() const { return locked() && valid_; }
  SensorTemperatureFormat format() const { return format_; }
  float temperature() const { return valid() ? temperature_ : NAN; }
  float humidity() const { return valid() ? humidity_ : NAN; }

 private:
  const SensorTemperatureFormat configured_;
  const bool humidityRaw16_;
  SensorTemperatureFormat format_ = SensorTemperatureFormat::Auto;
  uint8_t candidateMask_ = 0U;
  uint8_t consecutive_ = 0U;
  bool valid_ = false;
  bool hasLast_ = false;
  float lastTemperature_ = 0.0f;
  float temperature_ = NAN;
  float humidity_ = NAN;
};
