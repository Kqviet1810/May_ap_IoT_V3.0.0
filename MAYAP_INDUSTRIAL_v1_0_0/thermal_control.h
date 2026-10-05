#pragma once

#include "thermal_tune_candidate.h"

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

// AutoTune V4.3B production integration. These phases are service
// diagnostics only; public AutoTuneState values remain unchanged.
enum class AutoTunePhase : uint8_t {
  Idle, Precheck, CapturePower, ManualBaseline, IdentifyStep, ModelReady,
  Candidate, ValidationSettle, Validating, Accepted, Rejected, Failed,
  // Source-compatibility aliases for older host fixtures only.
  Preheat=CapturePower, Heating=IdentifyStep, Cooling=ValidationSettle,
  CycleValidating=ModelReady
};
enum class AutoTuneReason : uint8_t {
  None, SafetyAbort, SensorAbort, ModeAbort, PreheatTimeout, PhaseTimeout,
  TotalTimeout, NonRepeatable, AmplitudeTooSmall, PeriodTooSmall,
  InvalidKu, InvalidGains, InvalidRatios, BoundaryGain, CandidateReady,
  ValidationOvershoot, ValidationOscillation, ValidationBangBang,
  ValidationNoConvergence, ValidationWorse, ValidationTimeout,
  SaveFailed, Accepted,
  CaptureTimeout, BaselineTimeout, BaselineInputUnstable,
  InsufficientSafeExcitation, ModelInvalid, CandidateInvalid,
  ThermalGuard, ValidationSettleTimeout
};
inline const char *autoTunePhaseName(AutoTunePhase phase) {
  switch(phase) {
    case AutoTunePhase::Idle:return "IDLE";
    case AutoTunePhase::Precheck:return "PRECHECK";
    case AutoTunePhase::CapturePower:return "CAPTURE_POWER";
    case AutoTunePhase::ManualBaseline:return "MANUAL_BASELINE";
    case AutoTunePhase::IdentifyStep:return "IDENTIFY_STEP";
    case AutoTunePhase::ModelReady:return "MODEL_READY";
    case AutoTunePhase::Candidate:return "CANDIDATE";
    case AutoTunePhase::ValidationSettle:return "VALIDATION_SETTLE";
    case AutoTunePhase::Validating:return "VALIDATING";
    case AutoTunePhase::Accepted:return "ACCEPTED";
    case AutoTunePhase::Rejected:return "REJECTED";
    case AutoTunePhase::Failed:return "FAILED";
  }
  return "UNKNOWN";
}
inline const char *autoTuneReasonName(AutoTuneReason reason) {
  switch(reason) {
    case AutoTuneReason::None:return "NONE";
    case AutoTuneReason::SafetyAbort:return "SAFETY_ABORT";
    case AutoTuneReason::SensorAbort:return "SENSOR_ABORT";
    case AutoTuneReason::ModeAbort:return "MODE_ABORT";
    case AutoTuneReason::PreheatTimeout:return "PREHEAT_TIMEOUT";
    case AutoTuneReason::PhaseTimeout:return "PHASE_TIMEOUT";
    case AutoTuneReason::TotalTimeout:return "TOTAL_TIMEOUT";
    case AutoTuneReason::NonRepeatable:return "NON_REPEATABLE";
    case AutoTuneReason::AmplitudeTooSmall:return "AMPLITUDE_TOO_SMALL";
    case AutoTuneReason::PeriodTooSmall:return "PERIOD_TOO_SMALL";
    case AutoTuneReason::InvalidKu:return "INVALID_KU";
    case AutoTuneReason::InvalidGains:return "INVALID_GAINS";
    case AutoTuneReason::InvalidRatios:return "INVALID_RATIOS";
    case AutoTuneReason::BoundaryGain:return "BOUNDARY_GAIN";
    case AutoTuneReason::CandidateReady:return "CANDIDATE_READY";
    case AutoTuneReason::ValidationOvershoot:return "VALIDATION_OVERSHOOT";
    case AutoTuneReason::ValidationOscillation:return "VALIDATION_OSCILLATION";
    case AutoTuneReason::ValidationBangBang:return "VALIDATION_BANG_BANG";
    case AutoTuneReason::ValidationNoConvergence:return "VALIDATION_NO_CONVERGENCE";
    case AutoTuneReason::ValidationWorse:return "VALIDATION_WORSE";
    case AutoTuneReason::ValidationTimeout:return "VALIDATION_TIMEOUT";
    case AutoTuneReason::SaveFailed:return "SAVE_FAILED";
    case AutoTuneReason::Accepted:return "ACCEPTED";
    case AutoTuneReason::CaptureTimeout:return "CAPTURE_TIMEOUT";
    case AutoTuneReason::BaselineTimeout:return "MANUAL_BASELINE_TIMEOUT";
    case AutoTuneReason::BaselineInputUnstable:return "BASELINE_INPUT_UNSTABLE";
    case AutoTuneReason::InsufficientSafeExcitation:return "INSUFFICIENT_SAFE_EXCITATION";
    case AutoTuneReason::ModelInvalid:return "MODEL_INVALID";
    case AutoTuneReason::CandidateInvalid:return "CANDIDATE_INVALID";
    case AutoTuneReason::ThermalGuard:return "THERMAL_GUARD";
    case AutoTuneReason::ValidationSettleTimeout:return "VALIDATION_SETTLE_TIMEOUT";
  }
  return "UNKNOWN";
}

class RelayAutoTune {
 public:
  struct Cycle {
    float high=NAN,low=NAN,amplitude=0;
    uint32_t periodMs=0,heatMs=0,coolMs=0;
  };
  struct Result {
    float amplitude=0,periodSec=0,heatSec=0,coolSec=0,ku=0,gainScale=1;
    float baselineFraction=NAN,baselineSlope=NAN,stepFraction=NAN;
    float actualStep=NAN,tauC=NAN,Ti=NAN;
    ThermalPlantModel model{};
  };

  explicit RelayAutoTune(uint8_t unused=AUTOTUNE_PREHEAT_POWER_PERCENT) {
    (void)unused;
  }

  void configure(float target) { target_=target; }

  void start(uint32_t now,float input) {
    state_=AutoTuneState::Running;
    phase_=AutoTunePhase::Precheck;
    reason_=rejection_=AutoTuneReason::None;
    startedAt_=phaseStartedAt_=now;
    progress_=1;power_=0.0f;
    u0_=u1_=NAN;stepDirection_=0;
    baselineBucketCount_=0;
    baselineActualSum_=0.0f;
    baselineActualMin_=INFINITY;
    baselineActualMax_=-INFINITY;
    captureCount_=captureNext_=0;
    energyHead_=energyCount_=0;
    pvCount_=0;
    candidateGenerated_=false;
    model_=ThermalPlantModel{};
    candidate_=MachineConfig{};
    result_=Result{};
    lastCycle_=Cycle{};
    cycleSerial_=validationSerial_=0;
    meterStarted_=false;
    identifier_.reset();
    resetValidation();
    recordPv(now,input);
    if(!isfinite(input)||!isfinite(target_)) abort(AutoTuneReason::SensorAbort);
  }

  void abort(AutoTuneReason reason=AutoTuneReason::SafetyAbort) {
    if(state_==AutoTuneState::Success)return;
    state_=AutoTuneState::Failed;
    phase_=AutoTunePhase::Failed;
    reason_=rejection_=reason;
    power_=0.0f;
    progress_=0;
  }

  void cancel() {
    state_=AutoTuneState::Idle;
    phase_=AutoTunePhase::Idle;
    reason_=rejection_=AutoTuneReason::None;
    power_=0.0f;
    progress_=0;
    candidateGenerated_=false;
    meterStarted_=false;
    energyHead_=energyCount_=0;
  }

  // Meter the real arbiter SSR state. Requested PID/tune duty never enters
  // the identifier or capture-power statistics.
  void observeActual(uint32_t now,bool actualOn,float filteredPv) {
    if(!running()){meterStarted_=false;return;}
    if(!meterStarted_){
      meterStarted_=true;
      actualObservedAt_=bucketStartedAt_=now;
      bucketOnMs_=0U;
      lastActualOn_=actualOn;
      return;
    }
    uint32_t cursor=actualObservedAt_;
    uint32_t remain=static_cast<uint32_t>(now-actualObservedAt_);
    while(remain){
      const uint32_t bucketEnd=bucketStartedAt_+ENERGY_BUCKET_MS;
      const uint32_t toBoundary=static_cast<uint32_t>(bucketEnd-cursor);
      const uint32_t part=remain<toBoundary?remain:toBoundary;
      if(lastActualOn_)bucketOnMs_+=part;
      cursor+=part;
      remain-=part;
      if(cursor==bucketEnd){
        pushEnergy(bucketEnd,
                   static_cast<float>(bucketOnMs_)/static_cast<float>(ENERGY_BUCKET_MS),
                   filteredPv);
        bucketStartedAt_=bucketEnd;
        bucketOnMs_=0U;
      }
    }
    actualObservedAt_=now;
    lastActualOn_=actualOn;
  }

  void checkTimeout(uint32_t now) {
    if(!running())return;
    if(elapsedMs(now,startedAt_)>=FINAL_TOTAL_MAX_MS){
      reject(AutoTuneReason::TotalTimeout);
      return;
    }
    const uint32_t elapsed=elapsedMs(now,phaseStartedAt_);
    if(phase_==AutoTunePhase::CapturePower && elapsed>=CAPTURE_MAX_MS)
      reject(AutoTuneReason::CaptureTimeout);
    else if(phase_==AutoTunePhase::ManualBaseline && elapsed>=BASELINE_MAX_MS)
      reject(AutoTuneReason::BaselineTimeout);
    else if(phase_==AutoTunePhase::IdentifyStep && elapsed>=IDENTIFY_MAX_MS)
      reject(AutoTuneReason::ModelInvalid);
    else if(phase_==AutoTunePhase::ValidationSettle &&
            elapsed>=VALIDATION_SETTLE_MAX_MS)
      reject(AutoTuneReason::ValidationSettleTimeout);
    else if(phase_==AutoTunePhase::Validating &&
            elapsed>=VALIDATION_HARD_MAX_MS)
      reject(AutoTuneReason::ValidationTimeout);
  }

  bool update(uint32_t now,float input,const MachineConfig &cfg,
              MachineConfig &tunedOut,float phase1PredictedPeak=NAN,
              float highLimit=NAN) {
    if(!isfinite(highLimit)) highLimit=target_+0.7f;
    if(!running())return false;
    if(!isfinite(input)){abort(AutoTuneReason::SensorAbort);return false;}
    recordPv(now,input);
    checkTimeout(now);
    if(!running())return false;

    if(phase_==AutoTunePhase::Precheck){
      if(!isfinite(cfg.kp)||cfg.kp<=0.0f||cfg.maxHeaterPower==0U){
        reject(AutoTuneReason::CandidateInvalid);
        return false;
      }
      phase_=AutoTunePhase::CapturePower;
      phaseStartedAt_=now;
      progress_=5;
      restartEnergyWindow(now);
      return false;
    }

    if(phase_==AutoTunePhase::CapturePower){
      EnergySample sample{};
      while(popEnergy(sample)) pushCapture(sample.fraction);
      if(captureCount_<CAPTURE_BUCKETS)return false;
      const float mean=captureMean();
      if(!isfinite(mean)||mean<0.0f||
         mean>static_cast<float>(cfg.maxHeaterPower)*0.01f)
        return false;

      // SAFE TAKEOVER: no steady-PV requirement. Freeze LKG only when the
      // existing Phase-1 momentum estimate and the observed PV trend both say
      // a 120 s fixed-power baseline cannot coast into High.
      const float slope=safetySlope();
      const float slopePeak=input+fmaxf(0.0f,slope)*TAKEOVER_FORECAST_SEC;
      const float safePeak=fminf(highLimit-TAKEOVER_HIGH_MARGIN_C,
                                 target_+TAKEOVER_MAX_PEAK_ABOVE_SP_C);
      const float predicted=isfinite(phase1PredictedPeak)
          ? fmaxf(phase1PredictedPeak,slopePeak) : slopePeak;
      const float headroom=highLimit-input;
      const float safeHoldLimit=fminf(
          static_cast<float>(cfg.maxHeaterPower)*0.01f,
          TAKEOVER_MAX_HOLD_FRACTION);
      if(input<target_-CAPTURE_BELOW_SP_WINDOW_C ||
         input>target_+CAPTURE_ABOVE_SP_WINDOW_C ||
         mean>safeHoldLimit || headroom<CAPTURE_MIN_HEADROOM_C ||
         predicted>safePeak)
        return false;

      u0_=mean;
      result_.baselineFraction=u0_;
      power_=u0_*100.0f;
      phase_=AutoTunePhase::ManualBaseline;
      phaseStartedAt_=now;
      progress_=20;
      baselineBucketCount_=0;
      baselineActualSum_=0.0f;
      baselineActualMin_=INFINITY;
      baselineActualMax_=-INFINITY;
      identifier_.reset();             // reset only AFTER fixed hold is selected
      restartEnergyWindow(now);
      return false;
    }

    if(phase_==AutoTunePhase::ManualBaseline){
      power_=u0_*100.0f;
      if(!thermalGuard(input,highLimit,false)){
        reject(AutoTuneReason::ThermalGuard);
        return false;
      }
      EnergySample sample{};
      while(popEnergy(sample)){
        if(!identifier_.addSample(sample.at,sample.pv,sample.fraction)){
          reject(AutoTuneReason::BaselineInputUnstable);
          return false;
        }
        baselineActualSum_+=sample.fraction;
        baselineActualMin_=fminf(baselineActualMin_,sample.fraction);
        baselineActualMax_=fmaxf(baselineActualMax_,sample.fraction);
        if(baselineBucketCount_<255U)++baselineBucketCount_;
      }
      if(baselineBucketCount_<BASELINE_BUCKETS)return false;

      const float actualMean=baselineActualSum_/baselineBucketCount_;
      if(!isfinite(actualMean) ||
         fabsf(baselineActualMin_-actualMean)>BASELINE_INPUT_TOLERANCE ||
         fabsf(baselineActualMax_-actualMean)>BASELINE_INPUT_TOLERANCE){
        reject(AutoTuneReason::BaselineInputUnstable);
        return false;
      }

      // Prefer cooling/de-energizing excitation. A positive step is only a
      // fallback when the measured baseline cannot provide enough negative dU.
      const float negativeMagnitude=fminf(MAX_STEP_FRACTION,actualMean);
      if(negativeMagnitude>=MIN_STEP_FRACTION){
        stepDirection_=-1;
        u1_=fmaxf(0.0f,actualMean-negativeMagnitude);
      } else {
        const float maximum=static_cast<float>(cfg.maxHeaterPower)*0.01f;
        const float positiveMagnitude=fminf(MAX_STEP_FRACTION,maximum-actualMean);
        if(positiveMagnitude<MIN_STEP_FRACTION ||
           highLimit-input<POSITIVE_MIN_HEADROOM_C ||
           safetySlope()>POSITIVE_MAX_RISING_SLOPE){
          reject(AutoTuneReason::InsufficientSafeExcitation);
          return false;
        }
        stepDirection_=1;
        u1_=actualMean+positiveMagnitude;
      }

      stepStartPv_=input;
      power_=u1_*100.0f;
      relayLow_=actualMean*100.0f;
      relayHigh_=u1_*100.0f;
      result_.stepFraction=u1_;
      phase_=AutoTunePhase::IdentifyStep;
      phaseStartedAt_=now;
      progress_=45;
      restartEnergyWindow(now);
      return false;
    }

    if(phase_==AutoTunePhase::IdentifyStep){
      power_=u1_*100.0f;
      if(!thermalGuard(input,highLimit,stepDirection_>0)){
        reject(AutoTuneReason::ThermalGuard);
        return false;
      }
      if(stepDirection_<0 && stepStartPv_-input>NEGATIVE_MAX_EXCURSION_C){
        reject(AutoTuneReason::ThermalGuard);
        return false;
      }

      EnergySample sample{};
      while(popEnergy(sample)){
        if(!identifier_.addSample(sample.at,sample.pv,sample.fraction)){
          reject(AutoTuneReason::ModelInvalid);
          return false;
        }
      }
      model_=identifier_.identify();
      if(!model_.valid)return false;

      result_.model=model_;
      result_.baselineSlope=model_.baselineSlope;
      result_.actualStep=model_.actualStep;
      ++cycleSerial_;
      phase_=AutoTunePhase::ModelReady;
      phaseStartedAt_=now;
      power_=u0_*100.0f;
      progress_=72;
      return false;
    }

    if(phase_==AutoTunePhase::ModelReady){
      float tauC=NAN;
      if(model_.mode==ThermalPlantModelMode::FOPDT &&
         isfinite(model_.processGain)&&model_.processGain>0.0f &&
         isfinite(model_.tauSec)&&model_.tauSec>0.0f){
        tauC=100.0f*model_.tauSec/(model_.processGain*cfg.kp)-model_.thetaSec;
      } else if(model_.mode==ThermalPlantModelMode::SlowSlope &&
                isfinite(model_.kPrime)&&model_.kPrime>0.0f){
        tauC=100.0f/(model_.kPrime*cfg.kp)-model_.thetaSec;
      }
      if(!isfinite(tauC)){
        reject(AutoTuneReason::CandidateInvalid);
        return false;
      }
      tauC=fmaxf(model_.thetaSec,tauC);
      const ThermalTuneCandidate generated=
          generateThermalSimcPiCandidate(model_,tauC);
      if(!generated.valid || !isfinite(generated.Kp) ||
         !isfinite(generated.Ki) || generated.Kd!=0.0f ||
         generated.Kp>cfg.kp+0.0005f){
        reject(AutoTuneReason::CandidateInvalid);
        return false;
      }

      MachineConfig candidate=cfg;
      candidate.controlMode=ControlMode::Pid;
      candidate.kp=generated.Kp;
      candidate.ki=generated.Ki;
      candidate.kd=0.0f;
      const float kp=candidate.kp,ki=candidate.ki;
      sanitizeMachineConfig(candidate);
      if(fabsf(candidate.kp-kp)>0.0001f ||
         fabsf(candidate.ki-ki)>0.0001f || candidate.kd!=0.0f){
        reject(AutoTuneReason::CandidateInvalid);
        return false;
      }

      candidate_=candidate;
      candidateGenerated_=true;
      result_.tauC=tauC;
      result_.Ti=generated.Ti;
      result_.gainScale=1.0f;
      phase_=AutoTunePhase::Candidate;
      reason_=AutoTuneReason::CandidateReady;
      progress_=78;
      tunedOut=candidate_;
      return true;
    }

    if(phase_==AutoTunePhase::ValidationSettle){
      power_=0.0f; // MachineController actively restores LKG + Phase-1 here.
      const float safePeak=fminf(highLimit-RECOVERY_HIGH_MARGIN_C,
                                 target_+RECOVERY_MAX_PEAK_ABOVE_SP_C);
      const bool peakSafe=!isfinite(phase1PredictedPeak) ||
                          phase1PredictedPeak<=safePeak;
      const bool pvReady=input>=target_-RECOVERY_BELOW_SP_C &&
                         input<=target_+RECOVERY_ABOVE_SP_C;
      // Recovery is active, not a passive "thermal steady" wait: no dT/dt=0
      // gate. The production LKG controller is allowed to be heating/cooling.
      if(elapsedMs(now,phaseStartedAt_)>=VALIDATION_SETTLE_MIN_MS &&
         pvReady && peakSafe){
        phase_=AutoTunePhase::Validating;
        phaseStartedAt_=now;
        reason_=AutoTuneReason::None;
        progress_=86;
        ++validationSerial_;
        resetValidation(input,now);
      }
      return false;
    }

    return false;
  }

  void beginValidation(uint32_t now,float input) {
    if(phase_!=AutoTunePhase::Candidate||!isfinite(input)){
      abort(AutoTuneReason::SensorAbort);
      return;
    }
    phase_=AutoTunePhase::ValidationSettle;
    phaseStartedAt_=now;
    reason_=AutoTuneReason::None;
    power_=0.0f;
    progress_=82;
    resetValidation(input,now);
  }

  // Candidate runs through the production PID/PDM/output route. Safety and
  // overshoot remain immediate. Slow thermal candidates receive a model-aware
  // observation duration; convergence is asymmetric because being below SP is
  // safe but may require passive plant time before the new PI can recover.
  void validate(uint32_t now,float input,float requested,float baselineRequested,
                float maxPower,bool actualOn) {
    if(phase_!=AutoTunePhase::Validating)return;
    if(!isfinite(input)||!isfinite(requested)||!isfinite(baselineRequested)||
       requested<0.0f||requested>maxPower){
      reject(AutoTuneReason::CandidateInvalid);
      return;
    }

    const float error=input-target_;
    const float absolute=fabsf(error);
    if(error>AUTOTUNE_VALIDATION_MAX_OVERSHOOT_C){
      reject(AutoTuneReason::ValidationOvershoot);
      return;
    }

    validationSumAbs_+=absolute;
    validationSumError_+=error;
    ++validationSamples_;
    validationMin_=fminf(validationMin_,input);
    validationMax_=fmaxf(validationMax_,input);
    const bool extreme=requested<=0.5f||requested>=maxPower-0.5f;
    if(extreme)++validationExtreme_;
    if(extreme && baselineRequested>maxPower*0.10f &&
       baselineRequested<maxPower*0.90f)
      ++validationBaselineModerate_;
    if(actualOn!=validationLastOn_){
      ++validationTransitions_;
      validationLastOn_=actualOn;
    }

    if(elapsedMs(now,validationWindowAt_)<AUTOTUNE_VALIDATION_WINDOW_MS)
      return;

    const float mean=validationSamples_
        ? validationSumAbs_/validationSamples_ : INFINITY;
    const float signedMean=validationSamples_
        ? validationSumError_/validationSamples_ : INFINITY;
    const float range=validationMax_-validationMin_;
    const float extremeFraction=validationSamples_
        ? static_cast<float>(validationExtreme_)/validationSamples_ : 1.0f;
    const float worseFraction=validationSamples_
        ? static_cast<float>(validationBaselineModerate_)/validationSamples_ : 1.0f;

    if(range>VALIDATION_MAX_WINDOW_RANGE_C){
      reject(AutoTuneReason::ValidationOscillation);
      return;
    }
    if(extremeFraction>0.90f && mean>0.25f){
      reject(AutoTuneReason::ValidationBangBang);
      return;
    }
    if(worseFraction>0.60f && mean>0.20f){
      reject(AutoTuneReason::ValidationWorse);
      return;
    }

    ++validationWindow_;
    const uint32_t duration=validationDurationMs();
    if(elapsedMs(now,validationStartedAt_)>=duration){
      if(signedMean>VALIDATION_MAX_HOT_MEAN_ERROR_C ||
         signedMean< -VALIDATION_MAX_COLD_MEAN_ERROR_C ||
         range>VALIDATION_MAX_FINAL_RANGE_C){
        reject(AutoTuneReason::ValidationNoConvergence);
        return;
      }
      state_=AutoTuneState::Success;
      phase_=AutoTunePhase::Accepted;
      reason_=AutoTuneReason::Accepted;
      power_=0.0f;
      progress_=100;
      return;
    }

    validationWindowAt_=now;
    validationSamples_=validationExtreme_=validationBaselineModerate_=0;
    validationSumAbs_=validationSumError_=0.0f;
    validationMin_=validationMax_=input;
    progress_=static_cast<uint8_t>(86U+
        std::min<uint32_t>(13U,elapsedMs(now,validationStartedAt_)*13U/
                           std::max<uint32_t>(1U,duration)));
  }

  AutoTuneState state()const{return state_;}
  AutoTunePhase phase()const{return phase_;}
  AutoTuneReason reason()const{return reason_;}
  AutoTuneReason rejection()const{return rejection_;}
  uint8_t progress()const{return progress_;}
  float power()const{return power_;}
  float relayHigh()const{return relayHigh_;}
  float relayLow()const{return relayLow_;}
  uint8_t cycleCount()const{return 0U;}
  uint32_t cycleSerial()const{return cycleSerial_;}
  uint32_t validationSerial()const{return validationSerial_;}
  uint32_t preheatMs()const{return captureElapsedMs_;}
  uint32_t firstUpperMs()const{return 0U;}
  const Cycle &lastCycle()const{return lastCycle_;}
  const Result &result()const{return result_;}
  const MachineConfig &candidate()const{return candidate_;}
  const ThermalPlantModel &model()const{return model_;}
  bool validating()const{return phase_==AutoTunePhase::Validating;}
  bool validationSettling()const{return phase_==AutoTunePhase::ValidationSettle;}
  bool capturePowerActive()const{return phase_==AutoTunePhase::CapturePower;}
  bool manualExcitationActive()const{
    return phase_==AutoTunePhase::ManualBaseline ||
           phase_==AutoTunePhase::IdentifyStep ||
           phase_==AutoTunePhase::ModelReady;
  }
  bool candidateGenerated()const{return candidateGenerated_;}
  bool running()const{return state_==AutoTuneState::Running;}
  int8_t stepDirection()const{return stepDirection_;}
  float baselineFraction()const{return u0_;}
  float stepFraction()const{return u1_;}

 private:
  struct EnergySample {
    uint32_t at=0;
    float fraction=0.0f;
    float pv=NAN;
  };

  static constexpr uint32_t ENERGY_BUCKET_MS=10000U;
  static constexpr uint8_t ENERGY_QUEUE=8U,CAPTURE_BUCKETS=6U;
  static constexpr uint8_t BASELINE_BUCKETS=12U,PV_WINDOW=16U;
  static constexpr uint32_t FINAL_TOTAL_MAX_MS=80UL*60UL*1000UL;
  static constexpr uint32_t CAPTURE_MAX_MS=30UL*60UL*1000UL;
  static constexpr uint32_t BASELINE_MAX_MS=5UL*60UL*1000UL;
  static constexpr uint32_t IDENTIFY_MAX_MS=15UL*60UL*1000UL;
  static constexpr uint32_t VALIDATION_SETTLE_MIN_MS=60000UL;
  static constexpr uint32_t VALIDATION_SETTLE_MAX_MS=15UL*60UL*1000UL;
  static constexpr uint32_t VALIDATION_HARD_MAX_MS=12UL*60UL*1000UL;
  static constexpr float CAPTURE_MIN_HEADROOM_C=0.60f;
  static constexpr float CAPTURE_BELOW_SP_WINDOW_C=5.0f;
  static constexpr float CAPTURE_ABOVE_SP_WINDOW_C=0.15f;
  static constexpr float TAKEOVER_MAX_HOLD_FRACTION=0.35f;
  static constexpr float TAKEOVER_FORECAST_SEC=120.0f;
  static constexpr float TAKEOVER_HIGH_MARGIN_C=0.40f;
  static constexpr float TAKEOVER_MAX_PEAK_ABOVE_SP_C=0.20f;
  static constexpr float BASELINE_INPUT_TOLERANCE=0.025f;
  static constexpr float MAX_STEP_FRACTION=0.10f;
  static constexpr float MIN_STEP_FRACTION=0.08f;
  static constexpr float POSITIVE_MIN_HEADROOM_C=0.80f;
  static constexpr float POSITIVE_MAX_RISING_SLOPE=0.003f;
  static constexpr float GUARD_MARGIN_TO_HIGH_C=0.25f;
  static constexpr float GUARD_RISING_SLOPE=0.010f;
  static constexpr float GUARD_RISING_HEADROOM_C=0.60f;
  static constexpr float GUARD_FORECAST_SEC=90.0f;
  static constexpr float POSITIVE_MAX_EXCURSION_C=0.30f;
  static constexpr float NEGATIVE_MAX_EXCURSION_C=2.0f;
  static constexpr float RECOVERY_BELOW_SP_C=0.50f;
  static constexpr float RECOVERY_ABOVE_SP_C=0.10f;
  static constexpr float RECOVERY_HIGH_MARGIN_C=0.40f;
  static constexpr float RECOVERY_MAX_PEAK_ABOVE_SP_C=0.20f;
  static constexpr float VALIDATION_MAX_WINDOW_RANGE_C=0.35f;
  static constexpr float VALIDATION_MAX_FINAL_RANGE_C=0.25f;
  static constexpr float VALIDATION_MAX_HOT_MEAN_ERROR_C=0.25f;
  static constexpr float VALIDATION_MAX_COLD_MEAN_ERROR_C=0.45f;

  void reject(AutoTuneReason reason){
    state_=AutoTuneState::Failed;
    phase_=AutoTunePhase::Rejected;
    reason_=rejection_=reason;
    power_=0.0f;
    progress_=0;
  }

  void pushEnergy(uint32_t at,float fraction,float pv){
    if(energyCount_>=ENERGY_QUEUE){
      energyHead_=(energyHead_+1U)%ENERGY_QUEUE;
      --energyCount_;
    }
    const uint8_t slot=(energyHead_+energyCount_)%ENERGY_QUEUE;
    energy_[slot].at=at;
    energy_[slot].fraction=clampFloat(fraction,0.0f,1.0f);
    energy_[slot].pv=pv;
    ++energyCount_;
  }

  bool popEnergy(EnergySample &out){
    if(!energyCount_)return false;
    out=energy_[energyHead_];
    energyHead_=(energyHead_+1U)%ENERGY_QUEUE;
    --energyCount_;
    return true;
  }

  void restartEnergyWindow(uint32_t now){
    energyHead_=energyCount_=0U;
    bucketStartedAt_=actualObservedAt_=now;
    bucketOnMs_=0U;
  }

  void pushCapture(float fraction){
    capture_[captureNext_]=fraction;
    captureNext_=(captureNext_+1U)%CAPTURE_BUCKETS;
    if(captureCount_<CAPTURE_BUCKETS)++captureCount_;
    captureElapsedMs_=elapsedMs(actualObservedAt_,startedAt_);
  }

  float captureMean()const{
    if(!captureCount_)return NAN;
    double sum=0.0;
    for(uint8_t i=0;i<captureCount_;++i)sum+=capture_[i];
    return static_cast<float>(sum/captureCount_);
  }

  void recordPv(uint32_t now,float input){
    if(!isfinite(input))return;
    if(pvCount_<PV_WINDOW){
      pvAt_[pvCount_]=now;
      pv_[pvCount_]=input;
      ++pvCount_;
    } else {
      for(uint8_t i=1;i<PV_WINDOW;++i){
        pvAt_[i-1U]=pvAt_[i];
        pv_[i-1U]=pv_[i];
      }
      pvAt_[PV_WINDOW-1U]=now;
      pv_[PV_WINDOW-1U]=input;
    }
  }

  float safetySlope()const{
    if(pvCount_<2U)return 0.0f;
    const float dt=static_cast<float>(
        elapsedMs(pvAt_[pvCount_-1U],pvAt_[0]))*0.001f;
    return dt>0.0f?(pv_[pvCount_-1U]-pv_[0])/dt:0.0f;
  }

  bool thermalGuard(float input,float highLimit,bool positive)const{
    const float headroom=highLimit-input;
    if(headroom<=GUARD_MARGIN_TO_HIGH_C)return false;
    const float slope=safetySlope();
    const float slopePeak=input+fmaxf(0.0f,slope)*GUARD_FORECAST_SEC;
    if(slopePeak>=highLimit-GUARD_MARGIN_TO_HIGH_C)return false;
    if(slope>GUARD_RISING_SLOPE && headroom<GUARD_RISING_HEADROOM_C)
      return false;
    if(positive &&
       (headroom<POSITIVE_MIN_HEADROOM_C ||
        input-stepStartPv_>POSITIVE_MAX_EXCURSION_C))
      return false;
    return true;
  }

  uint32_t validationDurationMs()const{
    const float seconds=clampFloat(result_.tauC*4.0f,180.0f,600.0f);
    return static_cast<uint32_t>(seconds*1000.0f);
  }

  void resetValidation(float input=NAN,uint32_t now=0U){
    validationStartedAt_=validationWindowAt_=now;
    validationSamples_=0;
    validationExtreme_=validationBaselineModerate_=validationTransitions_=0;
    validationWindow_=0;
    validationSumAbs_=validationSumError_=0.0f;
    validationMin_=validationMax_=isfinite(input)?input:0.0f;
    validationLastOn_=false;
  }

  AutoTuneState state_=AutoTuneState::Idle;
  AutoTunePhase phase_=AutoTunePhase::Idle;
  AutoTuneReason reason_=AutoTuneReason::None,rejection_=AutoTuneReason::None;
  float target_=37.5f,power_=0.0f,relayHigh_=0.0f,relayLow_=0.0f;
  float u0_=NAN,u1_=NAN,stepStartPv_=NAN;
  int8_t stepDirection_=0;
  uint32_t startedAt_=0,phaseStartedAt_=0,captureElapsedMs_=0;
  uint32_t cycleSerial_=0,validationSerial_=0;
  uint8_t progress_=0;
  bool candidateGenerated_=false;

  bool meterStarted_=false,lastActualOn_=false;
  uint32_t actualObservedAt_=0,bucketStartedAt_=0,bucketOnMs_=0;
  EnergySample energy_[ENERGY_QUEUE]{};
  uint8_t energyHead_=0,energyCount_=0;

  float capture_[CAPTURE_BUCKETS]{};
  uint8_t captureCount_=0,captureNext_=0;
  uint8_t baselineBucketCount_=0;
  float baselineActualSum_=0.0f;
  float baselineActualMin_=INFINITY,baselineActualMax_=-INFINITY;

  uint32_t pvAt_[PV_WINDOW]{};
  float pv_[PV_WINDOW]{};
  uint8_t pvCount_=0;

  ThermalPlantIdentifier identifier_{};
  ThermalPlantModel model_{};
  ThermalTuneCandidate generated_{};
  MachineConfig candidate_{};
  Cycle lastCycle_{};
  Result result_{};

  uint32_t validationStartedAt_=0,validationWindowAt_=0;
  uint16_t validationSamples_=0,validationExtreme_=0;
  uint16_t validationBaselineModerate_=0,validationTransitions_=0;
  uint8_t validationWindow_=0;
  float validationSumAbs_=0.0f,validationSumError_=0.0f;
  float validationMin_=0.0f,validationMax_=0.0f;
  bool validationLastOn_=false;
};

