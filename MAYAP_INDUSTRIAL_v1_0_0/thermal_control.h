#pragma once

// Pure thermal algorithms. Including code provides MachineConfig, timing,
// constants and sanitizeMachineConfig; the host test runs these SAME classes.
//
// targetTemp is deliberately NOT part of this predicate. A setpoint edit is a
// control command and must reach the P term on the next sensor sample; treating
// it as a "bumpless config" change would back-calculate I to cancel that P step.
inline bool thermalPidRuntimeConfigChanged(const MachineConfig &before,
                                           const MachineConfig &after) {
  return before.controlMode != after.controlMode ||
         before.kp != after.kp ||
         before.ki != after.ki ||
         before.kd != after.kd ||
         before.maxHeaterPower != after.maxHeaterPower;
}

class ThermalController {
 public:
  explicit ThermalController(float beta = THERMAL_PID_BETA)
      : beta_(clampFloat(beta, 0.0f, 1.0f)) {}
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
      const float integralLimit = maxOut + fabsf(cfg.kp * (1.0f - beta_) * setpoint);
      integral_ = clampFloat(output_ - cfg.kp * (beta_ * setpoint - input),
                            -integralLimit, integralLimit);
    } else {
      integral_ = 0.0f;
    }
  }

  float updateOnNewSample(uint32_t now, float setpoint, float input,
                          const MachineConfig &cfg, bool enabled,
                          float actuatorCeiling = INFINITY, bool freezePositiveIntegral = false) {
    if (!enabled || !isfinite(input) || !isfinite(setpoint)) { reset(); return 0.0f; }
    const float maxOut = isfinite(actuatorCeiling)
        ? clampFloat(actuatorCeiling, 0.0f, static_cast<float>(cfg.maxHeaterPower))
        : static_cast<float>(cfg.maxHeaterPower);
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
      output_ = clampFloat(cfg.kp * (beta_ * setpoint - input), 0.0f, maxOut);
      return output_;
    }

    float dt = static_cast<float>(elapsedMs(now, lastComputeAt_)) * 0.001f;
    if (dt <= 0.0f) return output_;
    dt = clampFloat(dt, 0.25f, 10.0f);
    lastComputeAt_ = now;
    const float error = setpoint - input;
    const float dInput = (input - lastInput_) / dt;
    lastInput_ = input;

    const float p = cfg.kp * (beta_ * setpoint - input);
    // First-order derivative filter: sensor noise must not command full SSR
    // swings. Derivative stays on PV, avoiding setpoint derivative kick.
    filteredDerivative_ += (dt / (PID_D_FILTER_TAU_SEC + dt)) *
                           (dInput - filteredDerivative_);
    const float d = -cfg.kd * filteredDerivative_;
    // Weighted absolute Celsius P has a DC offset. Permit I to cancel it:
    // the old +/-maxOut bound alone can prevent beta<1 reaching the setpoint.
    // Anti-windup uses the ACTUAL 0..maxOut actuator limits.
    const float integralLimit = maxOut + fabsf(cfg.kp * (1.0f - beta_) * setpoint);
    const float integralDelta = cfg.ki * error * dt;
    const float candidateIntegral = clampFloat(integral_ +
        (freezePositiveIntegral && integralDelta > 0.0f ? 0.0f : integralDelta),
        -integralLimit, integralLimit);
    const float unsaturated = p + candidateIntegral + d;
    const float previousUnsaturated = p + integral_ + d;
    const float integralStep = candidateIntegral - integral_;
    // A step crossing a limit must reach that limit. Discarding the whole
    // step can leave positive heat indefinitely while PV is above SP.
    if (unsaturated < 0.0f && integralStep < 0.0f && previousUnsaturated > 0.0f) {
      integral_ = clampFloat(-p - d, -integralLimit, integralLimit);
    } else if (unsaturated > maxOut && integralStep > 0.0f &&
               previousUnsaturated < maxOut) {
      integral_ = clampFloat(maxOut - p - d, -integralLimit, integralLimit);
    } else if ((unsaturated >= 0.0f && unsaturated <= maxOut) ||
               (unsaturated > maxOut && integralStep < 0.0f) ||
               (unsaturated < 0.0f && integralStep > 0.0f)) {
      integral_ = candidateIntegral;
    }
    output_ = clampFloat(p + integral_ + d, 0.0f, maxOut);
    return output_;
  }

  float output() const { return output_; }

 private:
  const float beta_;
  bool initialized_ = false;
  float integral_ = 0.0f;
  float filteredDerivative_ = 0.0f;
  float lastInput_ = 0.0f;
  uint32_t lastComputeAt_ = 0;
  float output_ = 0.0f;
};

// Physical heat is metered from the arbiter's actual SSR state, not PID demand.
// The bounded 2 s buckets retain 240 s of energy and no credit across a cut.
class ThermalStartupController {
 public:
  enum class Phase : uint8_t { FullHeat, Approach, SoftLanding, Hold };
  void reset() {
    for (uint8_t i = 0; i < Buckets; ++i) onMs_[i] = 0;
    phase_ = Phase::FullHeat; initialized_ = false; observed_ = false; historyGap_ = false;
    bucket_ = 0; firstHeatAt_ = 0; firstRiseAt_ = 0;
    lastHeatOffAt_ = 0; energyWindowStartedAt_ = 0; coastCleared_ = false;
    stableAt_ = 0; lastRequested_ = 0; holdPower_ = 0;
    slope_ = 0; lastSampleAt_ = 0; lastPeak_ = 0;
  }
  void observe(uint32_t now, bool heaterOn) {
    if (!observed_) { observed_ = true; observedAt_ = bucketAt_ = now; lastOn_ = heaterOn; return; }
    uint32_t dt = static_cast<uint32_t>(now - observedAt_);
    observedAt_ = now;
    if (dt > 2000U) {
      reset(); historyGap_ = true;
      observed_ = true; observedAt_ = bucketAt_ = now; return;
    }
    while (dt) {
      const uint32_t left = 2000U - static_cast<uint32_t>(observedAt_ - dt - bucketAt_);
      const uint32_t part = dt < left ? dt : left;
      if (lastOn_) onMs_[bucket_] += part;
      dt -= part;
      if (part == left) {
        bucketAt_ += 2000U;
        bucket_ = (bucket_ + 1U) % Buckets;
        onMs_[bucket_] = 0;
      }
    }
    if (lastOn_ && !heaterOn) { lastHeatOffAt_ = now; coastCleared_ = false; }
    lastOn_ = heaterOn;
    if (heaterOn && firstHeatAt_ == 0U) firstHeatAt_ = now;
  }
  struct Decision { float ceiling; bool freezePositiveIntegral; Phase phase; float peak; };
  Decision decide(uint32_t now, float sp, float pv, float maxPower) {
    if (!isfinite(sp) || !isfinite(pv)) return {0, true, phase_, pv};
    historyGap_ = false; // only a fresh real sensor sample can resume heat
    if (!initialized_) {
      initialized_ = true; lastPv_ = pv; lastSp_ = sp;
      lastSampleAt_ = now;
    }
    const float dt = clampFloat(static_cast<float>(static_cast<uint32_t>(now-lastSampleAt_))*0.001f, 0.25f, 10.0f);
    const float measuredRate = (pv-lastPv_)/dt;
    slope_ += dt/(10.0f+dt)*(measuredRate-slope_);
    lastPv_ = pv; lastSampleAt_ = now;
    if (sp != lastSp_) { stableAt_ = 0U; phase_ = Phase::Approach; lastSp_ = sp; }
    if (firstRiseAt_ == 0U && firstHeatAt_ != 0U && slope_ > 0.003f &&
        static_cast<uint32_t>(now-firstHeatAt_) >= 15000U) firstRiseAt_ = now;
    const uint32_t riseDelayMs = firstRiseAt_ == 0U ? 0U :
        static_cast<uint32_t>(firstRiseAt_-firstHeatAt_);
    const bool longDelay = riseDelayMs > 80000U;
    const float coastSeconds = firstRiseAt_ == 0U ? 135.0f :
        clampFloat(static_cast<float>(riseDelayMs)*0.001f+14.0f, 18.0f, 135.0f);
    if (!coastCleared_ && !lastOn_ && lastHeatOffAt_ != 0U &&
        static_cast<uint32_t>(now-lastHeatOffAt_) >= static_cast<uint32_t>(coastSeconds*1000.0f) &&
        slope_ <= 0.001f) {
      for (uint8_t i = 0; i < Buckets; ++i) onMs_[i] = 0;
      energyWindowStartedAt_ = now;
      coastCleared_ = true;
    }
    const float slopeCoast = fmaxf(0.0f, slope_) * coastSeconds;
    // The long-term loss-compensation duty is deliberately slow and cannot
    // be learned until after the first delayed heat response. It removes the
    // permanent offset of proportional-only braking without banking pulse
    // energy for a later burst.
    if (firstRiseAt_ != 0U && (!longDelay || coastCleared_ || sp-pv > 1.0f) &&
        static_cast<uint32_t>(now-firstRiseAt_) >= (longDelay ? 240000U : 180000U)) {
      if (sp-pv > 0.08f && slope_ <= 0.003f)
        holdPower_ += dt/60.0f;
      else if (sp-pv < -0.05f || slope_ > 0.006f)
        holdPower_ -= 3.0f*dt/60.0f;
      holdPower_ = clampFloat(holdPower_,0.0f,maxPower);
    }
    const uint32_t horizonMs = longDelay ? 240000U : 160000U;
    const uint8_t recentBuckets = static_cast<uint8_t>(horizonMs/2000U);
    uint32_t recentOnMs = 0;
    for (uint8_t i = 0; i < recentBuckets; ++i)
      recentOnMs += onMs_[(bucket_+Buckets-i)%Buckets];
    const float windowMs = static_cast<float>(std::min<uint32_t>(horizonMs,
        static_cast<uint32_t>(now-energyWindowStartedAt_)));
    const float excessOnMs = fmaxf(0.0f, static_cast<float>(recentOnMs) -
        holdPower_ * 0.01f * windowMs);
    // Rated watts are not a calibration of transfer to this one probe.
    // Keep a bounded 35% reserve for heater effectiveness, sensor filter lag
    // and delayed heat before trusting a first-rise estimate.
    // Do not subtract PV rise over this ring: with transport delay, that rise
    // can be caused by an older pulse which already aged out of the ring.
    const float energyCoast = excessOnMs*16.0f*1.35f/MinimumCapacity;
    const float expectedCoast = fmaxf(slopeCoast, energyCoast);
    const float error = sp-pv;
    const float peak = pv+expectedCoast;
    lastPeak_ = peak;
    if (fabsf(error) <= 0.45f && fabsf(slope_) <= 0.002f) {
      if (stableAt_ == 0U) stableAt_ = now;
      if (static_cast<uint32_t>(now-stableAt_) >= 60000U) phase_ = Phase::Hold;
    } else stableAt_ = 0U;
    if (phase_ == Phase::Hold && (error > 0.7f || error < -0.25f || slope_ > 0.006f))
      phase_ = Phase::Approach;
    if (phase_ != Phase::Hold) {
      if (error > 2.0f && peak < sp-1.0f) phase_ = Phase::FullHeat;
      else if (error > 0.8f && peak < sp-0.3f) phase_ = Phase::Approach;
      else phase_ = Phase::SoftLanding;
    }
    // HOLD is still a delayed 16 kW plant. Never bypass the same braking
    // envelope merely because the filtered PV was momentarily stationary.
    const float remaining = error-expectedCoast;
    const float brakeFraction=clampFloat((remaining+0.05f)/0.5f, 0.0f, 1.0f);
    // The predictive fraction refers to the rated bank. External authority
    // remains a separate final limit and never redefines physical 100%.
    float cap = 100.0f*brakeFraction;
    // Near SP, a stationary heavy load can suddenly become light before the
    // remote sensor sees it. Allow only a bounded increment above established
    // maintenance duty until the changed slope is observed.
    if (error < 2.0f) cap = fminf(cap, fmaxf(20.0f,holdPower_+5.0f));
    if (slope_ > 0.0f) {
      const float rateCap = 100.0f*clampFloat(
          (error+0.1f)/(slope_*coastSeconds+0.1f), 0.0f, 1.0f);
      cap = fminf(cap, rateCap);
    }
    cap = clampFloat(cap, 0.0f, maxPower);
    cap = fminf(cap, lastRequested_+30.0f);
    const bool freezeIntegral = phase_ != Phase::Hold &&
        !(error > 0.15f && slope_ <= 0.001f && peak < sp-0.2f);
    return {cap, freezeIntegral, phase_, peak};
  }
  void requested(float power) { lastRequested_ = fmaxf(0.0f, power); }
  bool sampleStale(uint32_t now) const {
    return historyGap_ ||
        (initialized_ && static_cast<uint32_t>(now-lastSampleAt_) > 6000U);
  }
  Phase phase() const { return phase_; }
  float predictedPeak() const { return lastPeak_; }
 private:
  static constexpr uint8_t Buckets = 120U;
  static constexpr float MinimumCapacity = 180000.0f;
  uint32_t onMs_[Buckets]{};
  uint32_t observedAt_ = 0, bucketAt_ = 0;
  uint32_t firstHeatAt_ = 0, firstRiseAt_ = 0, lastHeatOffAt_ = 0;
  uint32_t energyWindowStartedAt_ = 0, stableAt_ = 0, lastSampleAt_ = 0;
  uint8_t bucket_ = 0;
  bool initialized_ = false, observed_ = false, lastOn_ = false, coastCleared_ = false;
  bool historyGap_ = false;
  float lastPv_ = 0, lastSp_ = 0, slope_ = 0;
  float lastRequested_ = 0, holdPower_ = 0, lastPeak_ = 0;
  Phase phase_ = Phase::FullHeat;
};

// These phases/reasons are service diagnostics, not changes to public state codes.
enum class AutoTunePhase : uint8_t {
  Idle, Preheat, Heating, Cooling, CycleValidating, Candidate, Validating,
  Accepted, Rejected, Failed
};
enum class AutoTuneReason : uint8_t {
  None, SafetyAbort, SensorAbort, ModeAbort, PreheatTimeout, PhaseTimeout,
  TotalTimeout, NonRepeatable, AmplitudeTooSmall, PeriodTooSmall,
  InvalidKu, InvalidGains, InvalidRatios, BoundaryGain, CandidateReady,
  ValidationOvershoot, ValidationOscillation, ValidationBangBang,
  ValidationNoConvergence, ValidationWorse, ValidationTimeout,
  SaveFailed, Accepted
};
inline const char *autoTunePhaseName(AutoTunePhase phase) {
  switch(phase) {
    case AutoTunePhase::Idle:return "IDLE";case AutoTunePhase::Preheat:return "PREHEAT";
    case AutoTunePhase::Heating:return "HEATING";case AutoTunePhase::Cooling:return "COOLING";
    case AutoTunePhase::CycleValidating:return "CYCLE_VALIDATING";case AutoTunePhase::Candidate:return "CANDIDATE";
    case AutoTunePhase::Validating:return "VALIDATING";case AutoTunePhase::Accepted:return "ACCEPTED";
    case AutoTunePhase::Rejected:return "REJECTED";case AutoTunePhase::Failed:return "FAILED";
  }
  return "UNKNOWN";
}
inline const char *autoTuneReasonName(AutoTuneReason reason) {
  switch(reason) {
    case AutoTuneReason::None:return "NONE";case AutoTuneReason::SafetyAbort:return "SAFETY_ABORT";
    case AutoTuneReason::SensorAbort:return "SENSOR_ABORT";case AutoTuneReason::ModeAbort:return "MODE_ABORT";
    case AutoTuneReason::PreheatTimeout:return "PREHEAT_TIMEOUT";case AutoTuneReason::PhaseTimeout:return "PHASE_TIMEOUT";
    case AutoTuneReason::TotalTimeout:return "TOTAL_TIMEOUT";case AutoTuneReason::NonRepeatable:return "NON_REPEATABLE";
    case AutoTuneReason::AmplitudeTooSmall:return "AMPLITUDE_TOO_SMALL";case AutoTuneReason::PeriodTooSmall:return "PERIOD_TOO_SMALL";
    case AutoTuneReason::InvalidKu:return "INVALID_KU";case AutoTuneReason::InvalidGains:return "INVALID_GAINS";
    case AutoTuneReason::InvalidRatios:return "INVALID_RATIOS";case AutoTuneReason::BoundaryGain:return "BOUNDARY_GAIN";
    case AutoTuneReason::CandidateReady:return "CANDIDATE_READY";
    case AutoTuneReason::ValidationOvershoot:return "VALIDATION_OVERSHOOT";
    case AutoTuneReason::ValidationOscillation:return "VALIDATION_OSCILLATION";
    case AutoTuneReason::ValidationBangBang:return "VALIDATION_BANG_BANG";
    case AutoTuneReason::ValidationNoConvergence:return "VALIDATION_NO_CONVERGENCE";
    case AutoTuneReason::ValidationWorse:return "VALIDATION_WORSE";
    case AutoTuneReason::ValidationTimeout:return "VALIDATION_TIMEOUT";
    case AutoTuneReason::SaveFailed:return "SAVE_FAILED";case AutoTuneReason::Accepted:return "ACCEPTED";
  }
  return "UNKNOWN";
}
class RelayAutoTune {
 public:
  struct Cycle { float high=NAN,low=NAN,amplitude=0; uint32_t periodMs=0,heatMs=0,coolMs=0; };
  struct Result { float amplitude=0,periodSec=0,heatSec=0,coolSec=0,ku=0,gainScale=1; };
  explicit RelayAutoTune(uint8_t preheatPercent=AUTOTUNE_PREHEAT_POWER_PERCENT)
      : preheatPercent_(preheatPercent) {}
  void configure(float target) { target_=target; }
  void start(uint32_t now,float input) {
    state_=AutoTuneState::Running;phase_=AutoTunePhase::Preheat;reason_=AutoTuneReason::None;
    rejection_=AutoTuneReason::None;startedAt_=phaseStartedAt_=now;preheatMs_=firstUpperMs_=0;
    hasUpper_=false;cycleCount_=0;cycleSerial_=validationSerial_=0;warmupDiscarded_=false;
    candidateGenerated_=false;
    currentLow_=currentHigh_=input;capturedHigh_=NAN;lastCycle_=Cycle{};result_=Result{};
    power_=relayHigh_=relayLow_=0;levelsLocked_=false;progress_=1;
    if(!isfinite(input) || !isfinite(target_))abort(AutoTuneReason::SensorAbort);
  }
  void abort(AutoTuneReason reason=AutoTuneReason::SafetyAbort) {
    state_=AutoTuneState::Failed;phase_=AutoTunePhase::Failed;reason_=reason;power_=0;progress_=0;
  }
  void cancel() {
    // Operator cancellation is not a tuning failure. Remove heater demand
    // immediately and return the public state to Idle; start() rebuilds all
    // measurement/cycle state before a later run.
    state_=AutoTuneState::Idle;phase_=AutoTunePhase::Idle;
    reason_=AutoTuneReason::None;rejection_=AutoTuneReason::None;
    power_=relayHigh_=relayLow_=0;progress_=0;levelsLocked_=false;
  }
  // Called every control cycle; timeout enforcement cannot wait for a sensor sample.
  void checkTimeout(uint32_t now) {
    if(!running())return;
    if(elapsedMs(now,startedAt_)>=AUTOTUNE_TOTAL_MAX_MS)abort(AutoTuneReason::TotalTimeout);
    else if(phase_==AutoTunePhase::Preheat && elapsedMs(now,phaseStartedAt_)>=AUTOTUNE_PREHEAT_MAX_MS)
      abort(AutoTuneReason::PreheatTimeout);
    else if(phase_!=AutoTunePhase::Preheat && elapsedMs(now,phaseStartedAt_)>=AUTOTUNE_PHASE_MAX_MS)
      abort(AutoTuneReason::PhaseTimeout);
  }
  bool update(uint32_t now,float input,const MachineConfig &cfg,MachineConfig &tunedOut) {
    if(!running())return false;
    if(!isfinite(input)){abort(AutoTuneReason::SensorAbort);return false;}
    checkTimeout(now);if(!running())return false;
    if(phase_==AutoTunePhase::Candidate||phase_==AutoTunePhase::Validating)return false;
    if(!levelsLocked_) {
      relayHigh_=static_cast<float>(std::min<uint8_t>(cfg.autotuneRelayPowerPercent,cfg.maxHeaterPower));
      relayLow_=0;levelsLocked_=true;
    }
    // Changing relay configuration during a measurement invalidates its swing.
    if(relayHigh_!=std::min<uint8_t>(cfg.autotuneRelayPowerPercent,cfg.maxHeaterPower)) {
      abort(AutoTuneReason::ModeAbort);return false;
    }
    if(!isfinite(cfg.autotuneBandC) || cfg.autotuneBandC<=0 || relayHigh_<=relayLow_) {
      abort(AutoTuneReason::InvalidKu);return false;
    }
    if(phase_==AutoTunePhase::Preheat) {
      power_=static_cast<float>(std::min<uint8_t>(preheatPercent_,cfg.maxHeaterPower));
      if(input<target_-cfg.autotuneBandC)return false;
      preheatMs_=elapsedMs(now,startedAt_);
      // No preheat peaks/periods enter measurement. Start fresh near lower band.
      phase_=input>=target_+cfg.autotuneBandC?AutoTunePhase::Cooling:AutoTunePhase::Heating;
      phaseStartedAt_=now;currentLow_=currentHigh_=input;capturedHigh_=NAN;
      power_=phase_==AutoTunePhase::Heating?relayHigh_:relayLow_;progress_=5;
      return false;
    }
    if(phase_==AutoTunePhase::Heating) {
      currentLow_=std::min(currentLow_,input);power_=relayHigh_;
      if(input<target_+cfg.autotuneBandC)return false;
      if(hasUpper_ && isfinite(capturedHigh_)) {
        lastCycle_.high=capturedHigh_;lastCycle_.low=currentLow_;
        lastCycle_.amplitude=(capturedHigh_-currentLow_)*0.5f;
        lastCycle_.periodMs=elapsedMs(now,lastUpperCrossAt_);
        lastCycle_.heatMs=elapsedMs(now,phaseStartedAt_);
        lastCycle_.coolMs=capturedCoolMs_;++cycleSerial_;
        if(lastCycle_.amplitude<AUTOTUNE_MIN_AMPLITUDE_C)rejection_=AutoTuneReason::AmplitudeTooSmall;
        else if(lastCycle_.periodMs<AUTOTUNE_MIN_PERIOD_MS)rejection_=AutoTuneReason::PeriodTooSmall;
        else if(!warmupDiscarded_)warmupDiscarded_=true;
        else {cycles_[cycleCount_++]=lastCycle_;progress_=static_cast<uint8_t>(cycleCount_*90U/AUTOTUNE_REQUIRED_CYCLES);}
      }
      if(!hasUpper_)firstUpperMs_=elapsedMs(now,startedAt_);
      hasUpper_=true;lastUpperCrossAt_=now;
      phase_=AutoTunePhase::Cooling;phaseStartedAt_=now;currentHigh_=input;power_=relayLow_;
    } else if(phase_==AutoTunePhase::Cooling) {
      currentHigh_=std::max(currentHigh_,input);power_=relayLow_;
      if(input>target_-cfg.autotuneBandC)return false;
      capturedHigh_=currentHigh_;capturedCoolMs_=elapsedMs(now,phaseStartedAt_);
      phase_=AutoTunePhase::Heating;phaseStartedAt_=now;currentLow_=input;power_=relayHigh_;
    }
    if(cycleCount_<AUTOTUNE_REQUIRED_CYCLES)return false;
    phase_=AutoTunePhase::CycleValidating;++validationSerial_;result_=Result{};
    for(uint8_t i=0;i<AUTOTUNE_REQUIRED_CYCLES;++i) {
      result_.amplitude+=cycles_[i].amplitude/AUTOTUNE_REQUIRED_CYCLES;
      result_.periodSec+=cycles_[i].periodMs*0.001f/AUTOTUNE_REQUIRED_CYCLES;
      result_.heatSec+=cycles_[i].heatMs*0.001f/AUTOTUNE_REQUIRED_CYCLES;
      result_.coolSec+=cycles_[i].coolMs*0.001f/AUTOTUNE_REQUIRED_CYCLES;
    }
    for(uint8_t i=0;i<AUTOTUNE_REQUIRED_CYCLES;++i) {
      if(fabsf(cycles_[i].amplitude-result_.amplitude)>result_.amplitude*AUTOTUNE_STABILITY_FRACTION ||
         fabsf(cycles_[i].periodMs*0.001f-result_.periodSec)>result_.periodSec*AUTOTUNE_STABILITY_FRACTION) {
        // Preserve rolling-window qualification; expose the rejected quality.
        rejection_=AutoTuneReason::NonRepeatable;
        for(uint8_t n=1;n<AUTOTUNE_REQUIRED_CYCLES;++n)cycles_[n-1]=cycles_[n];
        cycleCount_=AUTOTUNE_REQUIRED_CYCLES-1;phase_=AutoTunePhase::Cooling;return false;
      }
    }
    const float d=(relayHigh_-relayLow_)*0.5f; // Actual locked commanded swing, NEVER preheat.
    result_.ku=4*d/(static_cast<float>(PI)*result_.amplitude);
    if(!isfinite(result_.ku) || result_.ku<=0 || !isfinite(result_.periodSec) || result_.periodSec<=0) {
      abort(AutoTuneReason::InvalidKu);return false;
    }
    // Preserve Tyreus-Luyben coefficients and proportional gain scaling.
    const float kp=result_.ku/2.2f,ki=kp/(2.2f*result_.periodSec),kd=kp*result_.periodSec/6.3f;
    result_.gainScale=fmaxf(1.0f,fmaxf(kp/(100.0f*AUTOTUNE_GAIN_LIMIT_FRACTION),
        fmaxf(ki/(20.0f*AUTOTUNE_GAIN_LIMIT_FRACTION),kd/(200.0f*AUTOTUNE_GAIN_LIMIT_FRACTION))));
    // Round the common scale UP by one float ULP, so an exact boundary (e.g.
    // Kd=200) cannot divide to 200.000015 and be rejected/clipped by sanitize.
    if(result_.gainScale>1.0f && isfinite(result_.gainScale))
      result_.gainScale=nextafterf(result_.gainScale, INFINITY);
    if(!isfinite(kp) || !isfinite(ki) || !isfinite(kd) || !isfinite(result_.gainScale) ||
       kp/result_.gainScale<0.1f || ki<=0 || kd<=0) {abort(AutoTuneReason::InvalidGains);return false;}
    MachineConfig candidate=cfg;candidate.controlMode=ControlMode::Pid;
    candidate.kp=kp/result_.gainScale;candidate.ki=ki/result_.gainScale;candidate.kd=kd/result_.gainScale;
    if(candidate.kp>100 || candidate.ki>20 || candidate.kd>200) {abort(AutoTuneReason::InvalidGains);return false;}
    const float p=candidate.kp,i=candidate.ki,dGain=candidate.kd;
    sanitizeMachineConfig(candidate);
    if(candidate.kp!=p || candidate.ki!=i || candidate.kd!=dGain) {abort(AutoTuneReason::InvalidGains);return false;}
    candidate_=candidate;candidateGenerated_=true;phase_=AutoTunePhase::Candidate;
    reason_=AutoTuneReason::CandidateReady;power_=0;progress_=92;
    const float ti=candidate.kp/candidate.ki,td=candidate.kd/candidate.kp;
    if(!isfinite(ti)||!isfinite(td)||ti<20.0f||ti>1800.0f||td<1.0f||td>120.0f||
       result_.amplitude>AUTOTUNE_VALIDATION_MAX_OVERSHOOT_C*4.0f||result_.periodSec>900.0f) {
      reject(AutoTuneReason::InvalidRatios);return false;
    }
    // A candidate that removes nearly all P/I authority while raising D is
    // clearly worse than the known working gain set; do not trial it on heat.
    if(candidate.kp<cfg.kp*0.15f||candidate.ki<cfg.ki*0.01f) {
      reject(AutoTuneReason::InvalidRatios);return false;
    }
    if(candidate.kp>=95.0f||candidate.ki>=19.0f||candidate.kd>=190.0f) {
      reject(AutoTuneReason::BoundaryGain);return false;
    }
    tunedOut=candidate;return true;
  }
  void beginValidation(uint32_t now,float input) {
    if(phase_!=AutoTunePhase::Candidate||!isfinite(input)) {abort(AutoTuneReason::SensorAbort);return;}
    phase_=AutoTunePhase::Validating;reason_=AutoTuneReason::None;power_=0;progress_=94;
    validationStartedAt_=validationWindowAt_=now;validationSamples_=validationWindow_=0;
    validationSumAbs_=validationFirstMean_=validationPreviousRange_=0;
    validationMin_=validationMax_=input;validationExtreme_=validationBaselineModerate_=0;
    validationLastError_=input-target_;validationLastOn_=false;validationTransitions_=0;
  }
  void validate(uint32_t now,float input,float requested,float baselineRequested,
                float maxPower,bool actualOn) {
    if(phase_!=AutoTunePhase::Validating)return;
    if(!isfinite(input)||!isfinite(requested)||!isfinite(baselineRequested)||
       requested<0||requested>maxPower) {reject(AutoTuneReason::InvalidGains);return;}
    const float error=input-target_,absolute=fabsf(error);
    if(error>AUTOTUNE_VALIDATION_MAX_OVERSHOOT_C) {reject(AutoTuneReason::ValidationOvershoot);return;}
    validationSumAbs_+=absolute;++validationSamples_;
    validationMin_=fminf(validationMin_,input);validationMax_=fmaxf(validationMax_,input);
    const bool extreme=requested<=0.5f||requested>=maxPower-0.5f;
    if(extreme)++validationExtreme_;
    if(extreme&&baselineRequested>maxPower*0.10f&&baselineRequested<maxPower*0.90f)
      ++validationBaselineModerate_;
    if(actualOn!=validationLastOn_){++validationTransitions_;validationLastOn_=actualOn;}
    validationLastError_=error;
    if(elapsedMs(now,validationWindowAt_)<AUTOTUNE_VALIDATION_WINDOW_MS)return;
    const float mean=validationSamples_?validationSumAbs_/validationSamples_:INFINITY;
    const float range=validationMax_-validationMin_;
    const float extremeFraction=validationSamples_?static_cast<float>(validationExtreme_)/validationSamples_:1.0f;
    const float worseFraction=validationSamples_?static_cast<float>(validationBaselineModerate_)/validationSamples_:1.0f;
    if(validationWindow_==0)validationFirstMean_=mean;
    else if(range>validationPreviousRange_*1.35f+0.05f) {reject(AutoTuneReason::ValidationOscillation);return;}
    if(extremeFraction>0.90f&&mean>0.20f) {reject(AutoTuneReason::ValidationBangBang);return;}
    if(worseFraction>0.60f&&mean>0.15f) {reject(AutoTuneReason::ValidationWorse);return;}
    validationPreviousRange_=range;++validationWindow_;
    if(elapsedMs(now,validationStartedAt_)>=AUTOTUNE_VALIDATION_MS) {
      if(mean>0.25f||mean>validationFirstMean_*1.10f+0.02f) {
        reject(AutoTuneReason::ValidationNoConvergence);return;
      }
      state_=AutoTuneState::Success;phase_=AutoTunePhase::Accepted;
      reason_=AutoTuneReason::Accepted;power_=0;progress_=100;return;
    }
    validationWindowAt_=now;validationSamples_=validationExtreme_=validationBaselineModerate_=0;
    validationSumAbs_=0;validationMin_=validationMax_=input;
    progress_=static_cast<uint8_t>(94U+std::min<uint32_t>(5U,elapsedMs(now,validationStartedAt_)*5U/AUTOTUNE_VALIDATION_MS));
  }
  AutoTuneState state()const{return state_;}
  AutoTunePhase phase()const{return phase_;}
  AutoTuneReason reason()const{return reason_;}
  AutoTuneReason rejection()const{return rejection_;}
  uint8_t progress()const{return progress_;}
  float power()const{return power_;}
  float relayHigh()const{return relayHigh_;}
  float relayLow()const{return relayLow_;}
  uint8_t cycleCount()const{return cycleCount_;}
  uint32_t cycleSerial()const{return cycleSerial_;}
  uint32_t validationSerial()const{return validationSerial_;}
  uint32_t preheatMs()const{return preheatMs_;}
  uint32_t firstUpperMs()const{return firstUpperMs_;}
  const Cycle &lastCycle()const{return lastCycle_;}
  const Result &result()const{return result_;}
  const MachineConfig &candidate()const{return candidate_;}
  bool validating()const{return phase_==AutoTunePhase::Validating;}
  bool candidateGenerated()const{return candidateGenerated_;}
  bool running()const{return state_==AutoTuneState::Running;}
 private:
  void reject(AutoTuneReason reason){
    state_=AutoTuneState::Failed;phase_=AutoTunePhase::Rejected;reason_=reason;power_=0;progress_=0;
  }
  const uint8_t preheatPercent_;
  AutoTuneState state_=AutoTuneState::Idle;
  AutoTunePhase phase_=AutoTunePhase::Idle;
  AutoTuneReason reason_=AutoTuneReason::None,rejection_=AutoTuneReason::None;
  float target_=37.5f,power_=0,relayHigh_=0,relayLow_=0;
  uint32_t startedAt_=0,phaseStartedAt_=0,lastUpperCrossAt_=0,capturedCoolMs_=0;
  uint32_t preheatMs_=0,firstUpperMs_=0,cycleSerial_=0,validationSerial_=0;
  bool hasUpper_=false,levelsLocked_=false,warmupDiscarded_=false;
  float currentLow_=NAN,currentHigh_=NAN,capturedHigh_=NAN;
  Cycle cycles_[AUTOTUNE_REQUIRED_CYCLES]{};Cycle lastCycle_{};Result result_{};
  MachineConfig candidate_{};
  uint32_t validationStartedAt_=0,validationWindowAt_=0;
  uint16_t validationSamples_=0,validationExtreme_=0,validationBaselineModerate_=0,validationTransitions_=0;
  uint8_t validationWindow_=0;float validationSumAbs_=0,validationFirstMean_=0,validationPreviousRange_=0;
  float validationMin_=0,validationMax_=0,validationLastError_=0;bool validationLastOn_=false;
  bool candidateGenerated_=false;
  uint8_t cycleCount_=0,progress_=0;
};
