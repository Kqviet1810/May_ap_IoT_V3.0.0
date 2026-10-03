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
    const float candidateIntegral = clampFloat(
        integral_ + cfg.ki * error * dt, -integralLimit, integralLimit);
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

// These phases/reasons are service diagnostics, not changes to public state codes.
enum class AutoTunePhase : uint8_t { Idle, Preheat, Heating, Cooling, Validating, Success, Failed };
enum class AutoTuneReason : uint8_t {
  None, SafetyAbort, SensorAbort, ModeAbort, PreheatTimeout, PhaseTimeout,
  TotalTimeout, NonRepeatable, AmplitudeTooSmall, PeriodTooSmall,
  InvalidKu, InvalidGains, SaveFailed, Success
};
inline const char *autoTunePhaseName(AutoTunePhase phase) {
  switch(phase) {
    case AutoTunePhase::Idle:return "IDLE";case AutoTunePhase::Preheat:return "PREHEAT";
    case AutoTunePhase::Heating:return "HEATING";case AutoTunePhase::Cooling:return "COOLING";
    case AutoTunePhase::Validating:return "VALIDATING";case AutoTunePhase::Success:return "SUCCESS";
    case AutoTunePhase::Failed:return "FAILED";
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
    case AutoTuneReason::SaveFailed:return "SAVE_FAILED";case AutoTuneReason::Success:return "SUCCESS";
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
    currentLow_=currentHigh_=input;capturedHigh_=NAN;lastCycle_=Cycle{};result_=Result{};
    power_=relayHigh_=relayLow_=0;levelsLocked_=false;progress_=1;
    if(!isfinite(input) || !isfinite(target_))abort(AutoTuneReason::SensorAbort);
  }
  void abort(AutoTuneReason reason=AutoTuneReason::SafetyAbort) {
    state_=AutoTuneState::Failed;phase_=AutoTunePhase::Failed;reason_=reason;power_=0;progress_=0;
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
    phase_=AutoTunePhase::Validating;++validationSerial_;result_=Result{};
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
    result_.gainScale=fmaxf(1.0f,fmaxf(kp/100.0f,fmaxf(ki/20.0f,kd/200.0f)));
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
    tunedOut=candidate;state_=AutoTuneState::Success;phase_=AutoTunePhase::Success;
    reason_=AutoTuneReason::Success;power_=0;progress_=100;return true;
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
  bool running()const{return state_==AutoTuneState::Running;}
 private:
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
  uint8_t cycleCount_=0,progress_=0;
};
