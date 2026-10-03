#pragma once

// Pure thermal algorithms. Including code provides MachineConfig, timing,
// constants and sanitizeMachineConfig; the host test runs these SAME classes.
class ThermalController {
 public:
  void reset() {
    initialized_ = false;
    integral_ = 0.0f;
    lastInput_ = 0.0f;
    lastComputeAt_ = 0;
    output_ = 0.0f;
    filteredDerivative_ = 0.0f;
  }

  // Ap dung cau hinh moi ma giu nguyen cong suat hien tai. Cach nay tranh
  // nha contactor tong chi vi nguoi dung sua/lưu mot thong so tren HMI.
  void applyConfigBumpless(uint32_t now, float setpoint, float input,
                           const MachineConfig &cfg) {
    if (!initialized_ || !isfinite(input)) return;
    const float maxOut = static_cast<float>(cfg.maxHeaterPower);
    output_ = clampFloat(output_, 0.0f, maxOut);
    lastInput_ = input;
    lastComputeAt_ = now;
    filteredDerivative_ = 0.0f;
    if (cfg.controlMode == ControlMode::Pid) {
      const float error = setpoint - input;
      integral_ = clampFloat(output_ - cfg.kp * error, -maxOut, maxOut);
    } else {
      integral_ = 0.0f;
    }
  }

  float updateOnNewSample(uint32_t now, float setpoint, float input,
                          const MachineConfig &cfg, bool enabled) {
    if (!enabled || !isfinite(input) || !isfinite(setpoint)) { reset(); return 0.0f; }
    const float maxOut = static_cast<float>(cfg.maxHeaterPower);
    if (cfg.controlMode == ControlMode::OnOff) {
      const float half = cfg.tempHysteresis * 0.5f;
      if (!initialized_) { output_ = input < setpoint ? maxOut : 0.0f; initialized_ = true; }
      else if (input <= setpoint - half) output_ = maxOut;
      else if (input >= setpoint + half) output_ = 0.0f;
      lastInput_ = input; lastComputeAt_ = now;
      return output_;
    }

    if (!initialized_) {
      initialized_ = true;
      lastInput_ = input;
      lastComputeAt_ = now;
      integral_ = 0.0f;
      output_ = clampFloat(cfg.kp * (setpoint - input), 0.0f, maxOut);
      return output_;
    }

    float dt = static_cast<float>(elapsedMs(now, lastComputeAt_)) * 0.001f;
    if (dt <= 0.0f) return output_;
    dt = clampFloat(dt, 0.25f, 10.0f);
    lastComputeAt_ = now;
    const float error = setpoint - input;
    const float dInput = (input - lastInput_) / dt;
    lastInput_ = input;

    const float p = cfg.kp * error;
    // First-order derivative filter: sensor noise must not command full SSR
    // swings. Derivative stays on PV, avoiding setpoint derivative kick.
    filteredDerivative_ += (dt / (PID_D_FILTER_TAU_SEC + dt)) *
                           (dInput - filteredDerivative_);
    const float d = -cfg.kd * filteredDerivative_;
    const float candidateIntegral = clampFloat(
        integral_ + cfg.ki * error * dt, -maxOut, maxOut);
    const float unsaturated = p + candidateIntegral + d;
    // Tich phan co dieu kien: chi tich khi chua bao hoa hoac dang keo khoi bao hoa.
    if ((unsaturated >= 0.0f && unsaturated <= maxOut) ||
        (unsaturated > maxOut && error < 0.0f) ||
        (unsaturated < 0.0f && error > 0.0f)) {
      integral_ = candidateIntegral;
    }
    output_ = clampFloat(p + integral_ + d, 0.0f, maxOut);
    return output_;
  }

  float output() const { return output_; }

 private:
  bool initialized_ = false;
  float integral_ = 0.0f;
  float filteredDerivative_ = 0.0f;
  float lastInput_ = 0.0f;
  uint32_t lastComputeAt_ = 0;
  float output_ = 0.0f;
};

class RelayAutoTune {
 public:
  void start(uint32_t now, float input) {
    state_ = AutoTuneState::Running;
    startedAt_ = now;
    phaseHeat_ = input < target_;
    phaseStartedAt_ = now;
    lastUpperCrossAt_ = 0;
    currentLow_ = input;
    currentHigh_ = input;
    capturedHigh_ = NAN;
    cycleCount_ = 0;
    warmupDiscarded_ = false;
    power_ = 0.0f;
    progress_ = 1;
  }
  void configure(float target) { target_ = target; }
  void abort() { state_ = AutoTuneState::Failed; power_ = 0.0f; progress_ = 0; }

  bool update(uint32_t now, float input, const MachineConfig &cfg,
              MachineConfig &tunedOut) {
    if (state_ != AutoTuneState::Running || !isfinite(input)) return false;
    if (elapsedMs(now, startedAt_) >= AUTOTUNE_MAX_MS ||
        elapsedMs(now, phaseStartedAt_) >= AUTOTUNE_PHASE_MAX_MS) {
      abort();
      return false;
    }

    if (phaseHeat_) {
      if (input < currentLow_) currentLow_ = input;
      power_ = static_cast<float>(std::min<uint8_t>(cfg.autotuneRelayPowerPercent,
                                               cfg.maxHeaterPower));
      if (input >= target_ + cfg.autotuneBandC) {
        if (isfinite(capturedHigh_) && lastUpperCrossAt_ != 0U) {
          const float amplitude = (capturedHigh_ - currentLow_) * 0.5f;
          const uint32_t period = elapsedMs(now, lastUpperCrossAt_);
          if (amplitude >= AUTOTUNE_MIN_AMPLITUDE_C &&
              period >= AUTOTUNE_MIN_PERIOD_MS) {
            // The first complete oscillation still contains startup transient.
            if (!warmupDiscarded_) {
              warmupDiscarded_ = true;
            } else {
              amplitudes_[cycleCount_] = amplitude;
              periodsMs_[cycleCount_] = period;
              ++cycleCount_;
              const uint16_t percent = static_cast<uint16_t>(
                  (static_cast<uint16_t>(cycleCount_) * 90U) /
                  AUTOTUNE_REQUIRED_CYCLES);
              progress_ = static_cast<uint8_t>(
                  std::min<uint16_t>(95U, percent));
            }
          }
        }
        lastUpperCrossAt_ = now;
        phaseHeat_ = false;
        phaseStartedAt_ = now;
        currentHigh_ = input;
        power_ = 0.0f;
      }
    } else {
      if (input > currentHigh_) currentHigh_ = input;
      power_ = 0.0f;
      if (input <= target_ - cfg.autotuneBandC) {
        capturedHigh_ = currentHigh_;
        phaseHeat_ = true;
        phaseStartedAt_ = now;
        currentLow_ = input;
      }
    }

    if (cycleCount_ >= AUTOTUNE_REQUIRED_CYCLES) {
      float amplitude = 0.0f;
      float periodSec = 0.0f;
      for (uint8_t i = 0; i < AUTOTUNE_REQUIRED_CYCLES; ++i) {
        amplitude += amplitudes_[i];
        periodSec += static_cast<float>(periodsMs_[i]) * 0.001f;
      }
      amplitude /= AUTOTUNE_REQUIRED_CYCLES;
      periodSec /= AUTOTUNE_REQUIRED_CYCLES;
      bool repeatable = true;
      for (uint8_t i = 0U; i < AUTOTUNE_REQUIRED_CYCLES; ++i) {
        repeatable = repeatable &&
            fabsf(amplitudes_[i] - amplitude) <= amplitude * AUTOTUNE_STABILITY_FRACTION &&
            fabsf(periodsMs_[i] * 0.001f - periodSec) <= periodSec * AUTOTUNE_STABILITY_FRACTION;
      }
      if (!repeatable) {
        // Keep a rolling window; never save gains from drifting/noisy cycles.
        for (uint8_t i = 1U; i < AUTOTUNE_REQUIRED_CYCLES; ++i) {
          amplitudes_[i - 1U] = amplitudes_[i];
          periodsMs_[i - 1U] = periodsMs_[i];
        }
        cycleCount_ = AUTOTUNE_REQUIRED_CYCLES - 1U;
        return false;
      }
      const float relayAmplitude = static_cast<float>(
          std::min<uint8_t>(cfg.autotuneRelayPowerPercent, cfg.maxHeaterPower)) * 0.5f;
      const float ku = (4.0f * relayAmplitude) /
                       (static_cast<float>(PI) * amplitude);
      if (!isfinite(ku) || ku <= 0.0f || periodSec <= 0.0f) {
        abort(); return false;
      }
      tunedOut = cfg;
      // Tyreus-Luyben PID: it gay vuot lo hon Ziegler-Nichols, hop he nhiet cham.
      const float kp = ku / 2.2f;
      const float ti = 2.2f * periodSec;
      const float td = periodSec / 6.3f;
      const float ki = kp / ti;
      const float kd = kp * td;
      // If gain limits are reached, reduce ALL gains proportionally. Preserve
      // Ti/Td instead of silently clipping only Kd and changing the controller.
      const float gainScale = fmaxf(1.0f, fmaxf(kp / 100.0f,
          fmaxf(ki / 20.0f, kd / 200.0f)));
      if (!isfinite(gainScale) || kp / gainScale < 0.1f) { abort(); return false; }
      tunedOut.controlMode = ControlMode::Pid;
      tunedOut.kp = kp / gainScale;
      tunedOut.ki = ki / gainScale;
      tunedOut.kd = kd / gainScale;
      sanitizeMachineConfig(tunedOut);
      state_ = AutoTuneState::Success;
      power_ = 0.0f;
      progress_ = 100;
      return true;
    }
    return false;
  }

  AutoTuneState state() const { return state_; }
  uint8_t progress() const { return progress_; }
  float power() const { return power_; }
  bool running() const { return state_ == AutoTuneState::Running; }

 private:
  AutoTuneState state_ = AutoTuneState::Idle;
  float target_ = 37.5f;
  uint32_t startedAt_ = 0;
  uint32_t phaseStartedAt_ = 0;
  bool phaseHeat_ = false;
  uint32_t lastUpperCrossAt_ = 0;
  float currentLow_ = NAN;
  float currentHigh_ = NAN;
  float capturedHigh_ = NAN;
  float amplitudes_[AUTOTUNE_REQUIRED_CYCLES]{};
  uint32_t periodsMs_[AUTOTUNE_REQUIRED_CYCLES]{};
  uint8_t cycleCount_ = 0;
  bool warmupDiscarded_ = false;
  uint8_t progress_ = 0;
  float power_ = 0.0f;
};
