#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

// Offline/commissioning-only thermal identification. The identifier consumes
// ACTUAL delivered heater fraction over each observation interval; it never
// consumes controller-requested duty and has no actuator/config/persistence API.
enum class ThermalPlantIdReason : uint8_t {
  Valid,
  InsufficientSamples,
  InvalidSample,
  NonMonotonicTime,
  NoActualStep,
  UnstableActualInput,
  NoResponse,
  ExcessiveNoise,
  NotSettled,
  MissingCrossing,
  InvalidParameters,
  PoorFit,
  CapacityExceeded
};

inline const char *thermalPlantIdReasonName(ThermalPlantIdReason reason) {
  switch (reason) {
    case ThermalPlantIdReason::Valid:return "VALID";
    case ThermalPlantIdReason::InsufficientSamples:return "INSUFFICIENT_SAMPLES";
    case ThermalPlantIdReason::InvalidSample:return "INVALID_SAMPLE";
    case ThermalPlantIdReason::NonMonotonicTime:return "NON_MONOTONIC_TIME";
    case ThermalPlantIdReason::NoActualStep:return "NO_ACTUAL_STEP";
    case ThermalPlantIdReason::UnstableActualInput:return "UNSTABLE_ACTUAL_INPUT";
    case ThermalPlantIdReason::NoResponse:return "NO_RESPONSE";
    case ThermalPlantIdReason::ExcessiveNoise:return "EXCESSIVE_NOISE";
    case ThermalPlantIdReason::NotSettled:return "NOT_SETTLED";
    case ThermalPlantIdReason::MissingCrossing:return "MISSING_CROSSING";
    case ThermalPlantIdReason::InvalidParameters:return "INVALID_PARAMETERS";
    case ThermalPlantIdReason::PoorFit:return "POOR_FIT";
    case ThermalPlantIdReason::CapacityExceeded:return "CAPACITY_EXCEEDED";
  }
  return "UNKNOWN";
}

enum class ThermalPlantModelMode : uint8_t { None, FOPDT, SlowSlope };
inline const char *thermalPlantModelModeName(ThermalPlantModelMode mode) {
  switch (mode) {
    case ThermalPlantModelMode::None:return "NONE";
    case ThermalPlantModelMode::FOPDT:return "FOPDT";
    case ThermalPlantModelMode::SlowSlope:return "SLOW_SLOPE";
  }
  return "UNKNOWN";
}

struct ThermalPlantModel {
  bool valid = false;
  ThermalPlantIdReason reason = ThermalPlantIdReason::InsufficientSamples;
  ThermalPlantModelMode mode = ThermalPlantModelMode::None;
  float processGain = NAN;       // degC per unit actual delivered heater fraction (FOPDT only)
  float tauSec = NAN;            // FOPDT only
  float thetaSec = NAN;
  float kPrime = NAN;            // K/tau, degC/s per unit heater fraction
  float residualRmseC = NAN;
  float normalizedResidual = NAN;
  float confidence = 0.0f;
  float actualStep = NAN;
  float deliveredOnSec = 0.0f;
  uint16_t samples = 0;          // total raw observations, saturated at uint16 max
};

// Converts raw physical SSR state into interval energy. This helper deliberately
// has no requested-duty input: delivered fraction is actual ON time / interval.
class ThermalDeliveredEnergyBucket {
 public:
  void reset(uint32_t timestampMs, bool actualSsrOn) {
    started_ = true;
    bucketStartMs_ = lastTimestampMs_ = timestampMs;
    actualOn_ = actualSsrOn;
    onMs_ = 0;
  }

  bool observe(uint32_t timestampMs, bool actualSsrOn) {
    if (!started_) { reset(timestampMs, actualSsrOn); return true; }
    const int32_t dt = static_cast<int32_t>(timestampMs - lastTimestampMs_);
    if (dt < 0) return false;
    if (actualOn_) onMs_ += static_cast<uint32_t>(dt);
    lastTimestampMs_ = timestampMs;
    actualOn_ = actualSsrOn;
    return true;
  }

  bool close(uint32_t timestampMs, float &actualDeliveredFraction) {
    if (!started_ || static_cast<int32_t>(timestampMs - lastTimestampMs_) < 0) return false;
    if (!observe(timestampMs, actualOn_)) return false;
    const uint32_t span = static_cast<uint32_t>(timestampMs - bucketStartMs_);
    if (span == 0U) return false;
    actualDeliveredFraction = static_cast<float>(onMs_) / static_cast<float>(span);
    if (actualDeliveredFraction < 0.0f) actualDeliveredFraction = 0.0f;
    if (actualDeliveredFraction > 1.0f) actualDeliveredFraction = 1.0f;
    bucketStartMs_ = timestampMs;
    onMs_ = 0;
    return true;
  }

 private:
  bool started_ = false;
  bool actualOn_ = false;
  uint32_t bucketStartMs_ = 0;
  uint32_t lastTimestampMs_ = 0;
  uint32_t onMs_ = 0;
};

class ThermalPlantIdentifier {
 public:
  // Staged retention keeps exact measured points (no waveform averaging): all
  // early samples, then progressively wider spacing once theta/early slope are
  // already observable. 832 points cover the existing ~9 h heavy reference
  // trace at 5 s input cadence while bounding permanent state near 10 KiB.
  static constexpr size_t MAX_RESPONSE_SAMPLES = 832U;
  static constexpr size_t BASELINE_WINDOW = 60U;

  void reset() { *this = ThermalPlantIdentifier(); }

  // actualDeliveredFraction is measured/known physical SSR ON-time fraction
  // over the observation interval [previous sample, this sample]. It is not a
  // raw SSR state and never controller-requested duty.
  bool addSample(uint32_t timestampMs, float filteredPv, float actualDeliveredFraction) {
    if (!std::isfinite(filteredPv) || !std::isfinite(actualDeliveredFraction) ||
        actualDeliveredFraction < 0.0f || actualDeliveredFraction > 1.0f) {
      terminalReason_ = ThermalPlantIdReason::InvalidSample; return false;
    }
    if (haveLast_ && static_cast<int32_t>(timestampMs - lastTimestampMs_) <= 0) {
      terminalReason_ = ThermalPlantIdReason::NonMonotonicTime; return false;
    }
    if (totalSamples_ < 0xffffffffU) ++totalSamples_;

    if (!stepDetected_) {
      if (haveLast_ && lastActual_ <= INPUT_OFF_MAX && actualDeliveredFraction > INPUT_OFF_MAX) {
        freezeBaseline();
        stepDetected_ = true;
        // The delivered fraction belongs to the interval ending at this sample,
        // so the physical step begins at the previous observation boundary.
        stepTimestampMs_ = lastTimestampMs_;
        deliveredOnSec_ += actualDeliveredFraction * static_cast<float>(timestampMs-lastTimestampMs_) * 0.001f;
        postInputMin_ = postInputMax_ = actualDeliveredFraction;
        appendResponse(timestampMs, filteredPv, actualDeliveredFraction);
      } else {
        pushBaseline(filteredPv, actualDeliveredFraction);
      }
    } else {
      const float dtSec = static_cast<float>(timestampMs - lastTimestampMs_) * 0.001f;
      deliveredOnSec_ += lastActual_ * dtSec;
      if (actualDeliveredFraction < postInputMin_) postInputMin_ = actualDeliveredFraction;
      if (actualDeliveredFraction > postInputMax_) postInputMax_ = actualDeliveredFraction;
      appendResponse(timestampMs, filteredPv, actualDeliveredFraction);
    }

    haveLast_ = true;
    lastTimestampMs_ = timestampMs;
    lastActual_ = actualDeliveredFraction;
    return terminalReason_ != ThermalPlantIdReason::CapacityExceeded;
  }

  ThermalPlantModel identify() const {
    ThermalPlantModel out;
    out.samples = totalSamples_ > 65535U ? 65535U : static_cast<uint16_t>(totalSamples_);
    out.deliveredOnSec = deliveredOnSec_;
    if (terminalReason_ == ThermalPlantIdReason::InvalidSample ||
        terminalReason_ == ThermalPlantIdReason::NonMonotonicTime ||
        terminalReason_ == ThermalPlantIdReason::CapacityExceeded) {
      out.reason = terminalReason_; return out;
    }
    if (!stepDetected_) { out.reason = totalSamples_ < MIN_TOTAL_SAMPLES ? ThermalPlantIdReason::InsufficientSamples : ThermalPlantIdReason::NoActualStep; return out; }
    if (baselineSamplesAtStep_ < MIN_BASELINE_SAMPLES || responseCount_ < MIN_RESPONSE_SAMPLES) return out;

    const float postSec = static_cast<float>(response_[responseCount_-1].timestampMs - stepTimestampMs_) * 0.001f;
    if (postSec <= 0.0f) return out;
    const float tailWindowSec = fminf(FINAL_WINDOW_MAX_SEC, postSec * 0.20f);
    if (tailWindowSec < MIN_FINAL_WINDOW_SEC) return out;
    const size_t finalStart = firstAtOrAfter(response_[responseCount_-1].timestampMs - static_cast<uint32_t>(tailWindowSec * 1000.0f));
    const float finalInput = meanResponseInput(finalStart, responseCount_);
    const float actualStep = finalInput - baselineInput_;
    out.actualStep = actualStep;
    if (!std::isfinite(actualStep) || actualStep < MIN_ACTUAL_STEP) { out.reason=ThermalPlantIdReason::NoActualStep; return out; }
    if (!baselineInputStable_) { out.reason=ThermalPlantIdReason::UnstableActualInput; return out; }
    const float inputTolerance=fmaxf(INPUT_STABILITY_ABS,actualStep*INPUT_STABILITY_FRACTION);
    if (std::fabs(postInputMin_-finalInput)>inputTolerance || std::fabs(postInputMax_-finalInput)>inputTolerance) {
      out.reason=ThermalPlantIdReason::UnstableActualInput; return out;
    }

    const float y0=baselinePv_;
    const float yFinal=meanResponsePv(finalStart,responseCount_);
    const float delta=yFinal-y0;
    if (!std::isfinite(delta) || delta < MIN_RESPONSE_C) { out.reason=ThermalPlantIdReason::NoResponse; return out; }
    if (baselineSigma_ > delta*MAX_NOISE_FRACTION) { out.reason=ThermalPlantIdReason::ExcessiveNoise; return out; }

    const uint32_t finalStartMs=response_[finalStart].timestampMs;
    const uint32_t endMs=response_[responseCount_-1].timestampMs;
    const size_t finalHalf=firstAtOrAfter(finalStartMs + (endMs-finalStartMs)/2U);
    if (finalHalf<=finalStart || finalHalf>=responseCount_) return out;
    const float finalDrift=std::fabs(meanResponsePv(finalHalf,responseCount_)-meanResponsePv(finalStart,finalHalf));
    const float settleLimit=fmaxf(MAX_FINAL_DRIFT_C,delta*MAX_FINAL_DRIFT_FRACTION);

    if (finalDrift <= settleLimit) {
      ThermalPlantModel fopdt = identifyFopdt(out, y0, delta, actualStep, finalDrift, postSec);
      if (fopdt.valid) return fopdt;
      if (fopdt.reason != ThermalPlantIdReason::MissingCrossing &&
          fopdt.reason != ThermalPlantIdReason::PoorFit) return fopdt;
      ThermalPlantModel slow = identifySlowSlope(out, y0, delta, actualStep, postSec);
      if (slow.valid) return slow;
      return fopdt;
    }

    ThermalPlantModel slow = identifySlowSlope(out, y0, delta, actualStep, postSec);
    if (slow.valid) return slow;
    out.reason=ThermalPlantIdReason::NotSettled;
    return out;
  }

 private:
  struct BaselineSample { float pv; float actual; };
  struct Sample { uint32_t timestampMs; float pv; float actual; };

  static constexpr size_t MIN_TOTAL_SAMPLES=40U,MIN_BASELINE_SAMPLES=12U,MIN_RESPONSE_SAMPLES=24U;
  static constexpr float FINAL_WINDOW_MAX_SEC=800.0f,MIN_FINAL_WINDOW_SEC=60.0f;
  static constexpr float INPUT_OFF_MAX=0.02f,MIN_ACTUAL_STEP=0.08f;
  static constexpr float INPUT_STABILITY_ABS=0.025f,INPUT_STABILITY_FRACTION=0.05f;
  static constexpr float MIN_RESPONSE_C=0.25f,MAX_NOISE_FRACTION=0.20f;
  static constexpr float MAX_FINAL_DRIFT_C=0.05f,MAX_FINAL_DRIFT_FRACTION=0.03f;
  static constexpr float MAX_NORMALIZED_RESIDUAL=0.15f;
  static constexpr float MIN_TAU_SEC=1.0f,MAX_TAU_SEC=86400.0f,MAX_THETA_SEC=21600.0f;
  static constexpr float MAX_PROCESS_GAIN=1000.0f;
  static constexpr float RETENTION_HIGH_RES_SEC=1200.0f,RETENTION_MID_RES_SEC=7200.0f;
  static constexpr uint32_t RETENTION_MID_SPACING_MS=20000U,RETENTION_LATE_SPACING_MS=100000U;
  static constexpr float SLOW_MIN_OBSERVATION_SEC=600.0f;
  static constexpr float SLOW_ONSET_MIN_C=0.08f;
  static constexpr float SLOW_REGRESSION_SEC=600.0f;
  static constexpr float SLOW_MIN_R2=0.94f;
  static constexpr float SLOW_MAX_NORMALIZED_RESIDUAL=0.18f;

  static float clamp01(float value){return value<0?0:value>1?1:value;}

  void pushBaseline(float pv,float actual) {
    if (baselineRingCount_ < BASELINE_WINDOW) {
      baseline_[baselineRingCount_++]={pv,actual};
    } else {
      baseline_[baselineRingNext_]={pv,actual};
      baselineRingNext_=(baselineRingNext_+1U)%BASELINE_WINDOW;
    }
    ++baselineTotal_;
    if (baselineTotal_==1U) baselineInputMin_=baselineInputMax_=actual;
    else { if(actual<baselineInputMin_)baselineInputMin_=actual; if(actual>baselineInputMax_)baselineInputMax_=actual; }
  }

  BaselineSample baselineAt(size_t chronological) const {
    if (baselineRingCount_ < BASELINE_WINDOW) return baseline_[chronological];
    return baseline_[(baselineRingNext_+chronological)%BASELINE_WINDOW];
  }

  void freezeBaseline() {
    baselineSamplesAtStep_=baselineTotal_;
    if (!baselineRingCount_) return;
    double pv=0,input=0;
    for(size_t i=0;i<baselineRingCount_;++i){const BaselineSample s=baselineAt(i);pv+=s.pv;input+=s.actual;}
    baselinePv_=static_cast<float>(pv/baselineRingCount_);
    baselineInput_=static_cast<float>(input/baselineRingCount_);
    double squared=0;
    for(size_t i=0;i<baselineRingCount_;++i){const double d=baselineAt(i).pv-baselinePv_;squared+=d*d;}
    baselineSigma_=sqrtf(static_cast<float>(squared/baselineRingCount_));
    baselineInputStable_=std::fabs(baselineInputMin_-baselineInput_)<=INPUT_STABILITY_ABS &&
                         std::fabs(baselineInputMax_-baselineInput_)<=INPUT_STABILITY_ABS;
  }

  uint32_t retentionSpacingMs(uint32_t timestampMs) const {
    const float postSec=static_cast<float>(timestampMs-stepTimestampMs_)*0.001f;
    if(postSec<=RETENTION_HIGH_RES_SEC)return 0U;
    if(postSec<=RETENTION_MID_RES_SEC)return RETENTION_MID_SPACING_MS;
    return RETENTION_LATE_SPACING_MS;
  }

  void appendResponse(uint32_t timestampMs,float pv,float actual) {
    const uint32_t spacing=retentionSpacingMs(timestampMs);
    if(responseCount_>0U && spacing>0U &&
       static_cast<uint32_t>(timestampMs-response_[responseCount_-1U].timestampMs)<spacing) return;
    if(responseCount_>=MAX_RESPONSE_SAMPLES) { terminalReason_=ThermalPlantIdReason::CapacityExceeded; return; }
    response_[responseCount_++]={timestampMs,pv,actual};
  }

  size_t firstAtOrAfter(uint32_t timestampMs) const {
    for(size_t i=0;i<responseCount_;++i) if(static_cast<int32_t>(response_[i].timestampMs-timestampMs)>=0) return i;
    return responseCount_-1U;
  }

  float meanResponsePv(size_t first,size_t end)const{
    double sum=0;for(size_t i=first;i<end;++i)sum+=response_[i].pv;
    return end>first?static_cast<float>(sum/(end-first)):NAN;
  }
  float meanResponseInput(size_t first,size_t end)const{
    double sum=0;for(size_t i=first;i<end;++i)sum+=response_[i].actual;
    return end>first?static_cast<float>(sum/(end-first)):NAN;
  }

  bool crossingTime(float y0,float delta,float level,float &seconds)const{
    for(size_t i=1;i+2U<responseCount_;++i){
      const float normalized=(response_[i].pv-y0)/delta;
      if(normalized<level||(response_[i+1U].pv-y0)/delta<level-0.02f||
         (response_[i+2U].pv-y0)/delta<level-0.02f)continue;
      const float previous=(response_[i-1U].pv-y0)/delta;
      const float fraction=normalized>previous?(level-previous)/(normalized-previous):0.0f;
      const float at=response_[i-1U].timestampMs+
          clamp01(fraction)*static_cast<float>(response_[i].timestampMs-response_[i-1U].timestampMs);
      seconds=(at-stepTimestampMs_)*0.001f;return true;
    }
    return false;
  }

  ThermalPlantModel identifyFopdt(ThermalPlantModel out,float y0,float delta,float actualStep,float finalDrift,float postSec) const {
    float t28=NAN,t63=NAN;
    if (!crossingTime(y0,delta,0.283f,t28) || !crossingTime(y0,delta,0.632f,t63)) {
      out.reason=ThermalPlantIdReason::MissingCrossing; return out;
    }
    const float tau=1.5f*(t63-t28);
    const float theta=fmaxf(0.0f,1.5f*t28-0.5f*t63);
    const float gain=delta/actualStep;
    if (!std::isfinite(gain)||!std::isfinite(tau)||!std::isfinite(theta)||
        gain<=0.0f||gain>MAX_PROCESS_GAIN||tau<MIN_TAU_SEC||tau>MAX_TAU_SEC||theta>MAX_THETA_SEC) {
      out.reason=ThermalPlantIdReason::InvalidParameters; return out;
    }
    double squared=0.0;
    for(size_t i=0;i<responseCount_;++i) {
      const float t=(response_[i].timestampMs-stepTimestampMs_)*0.001f;
      const float predicted=t<=theta?y0:y0+gain*actualStep*(1.0f-expf(-(t-theta)/tau));
      const float error=response_[i].pv-predicted;
      squared+=static_cast<double>(error)*error;
    }
    const float rmse=responseCount_?sqrtf(static_cast<float>(squared/responseCount_)):INFINITY;
    const float normalized=rmse/delta;
    if (!std::isfinite(normalized)||normalized>MAX_NORMALIZED_RESIDUAL) { out.reason=ThermalPlantIdReason::PoorFit; return out; }
    const float fitScore=clamp01(1.0f-normalized/MAX_NORMALIZED_RESIDUAL);
    const float durationScore=clamp01(postSec/(theta+4.0f*tau));
    const float stabilityScore=clamp01(1.0f-finalDrift/fmaxf(MAX_FINAL_DRIFT_C,delta*MAX_FINAL_DRIFT_FRACTION));
    out.valid=true;out.reason=ThermalPlantIdReason::Valid;out.mode=ThermalPlantModelMode::FOPDT;
    out.processGain=gain;out.tauSec=tau;out.thetaSec=theta;out.kPrime=gain/tau;
    out.residualRmseC=rmse;out.normalizedResidual=normalized;
    out.confidence=0.5f*fitScore+0.25f*durationScore+0.25f*stabilityScore;
    return out;
  }

  ThermalPlantModel identifySlowSlope(ThermalPlantModel out,float y0,float observedDelta,float actualStep,float postSec) const {
    if (postSec < SLOW_MIN_OBSERVATION_SEC) return out;
    const float onsetRise=fmaxf(SLOW_ONSET_MIN_C,baselineSigma_*4.0f);
    size_t onset=responseCount_;
    for(size_t i=1;i+2U<responseCount_;++i) {
      if(response_[i].pv-y0>=onsetRise && response_[i+1U].pv-y0>=onsetRise*0.8f && response_[i+2U].pv-y0>=onsetRise*0.8f){onset=i;break;}
    }
    if(onset==responseCount_) return out;
    const uint32_t regressionEndMs=response_[onset].timestampMs+static_cast<uint32_t>(SLOW_REGRESSION_SEC*1000.0f);
    size_t end=onset;
    while(end<responseCount_ && static_cast<int32_t>(response_[end].timestampMs-regressionEndMs)<=0) ++end;
    if(end-onset<8U) return out;

    double sw=0,st=0,sy=0,stt=0,sty=0;
    for(size_t i=onset;i<end;++i){
      const double w=1.0;
      const double t=(response_[i].timestampMs-stepTimestampMs_)*0.001;
      sw+=w;st+=w*t;sy+=w*response_[i].pv;stt+=w*t*t;sty+=w*t*response_[i].pv;
    }
    const double denom=sw*stt-st*st;
    if(!(denom>0.0)) return out;
    const double slope=(sw*sty-st*sy)/denom;
    const double intercept=(sy-slope*st)/sw;
    if(!std::isfinite(slope)||slope<=0.0) { out.reason=ThermalPlantIdReason::InvalidParameters; return out; }
    const double theta=(y0-intercept)/slope;
    const double kPrime=slope/actualStep;
    if(!std::isfinite(theta)||!std::isfinite(kPrime)||theta<0.0||theta>MAX_THETA_SEC||kPrime<=0.0||kPrime>MAX_PROCESS_GAIN) {
      out.reason=ThermalPlantIdReason::InvalidParameters; return out;
    }

    double squared=0,total=0,mean=sy/sw;
    for(size_t i=onset;i<end;++i){
      const double w=1.0;
      const double t=(response_[i].timestampMs-stepTimestampMs_)*0.001;
      const double predicted=intercept+slope*t;
      const double e=response_[i].pv-predicted;
      const double d=response_[i].pv-mean;
      squared+=w*e*e;total+=w*d*d;
    }
    const float rmse=sqrtf(static_cast<float>(squared/sw));
    const float regressionRise=static_cast<float>(slope*((response_[end-1U].timestampMs-response_[onset].timestampMs)*0.001));
    const float normalized=rmse/fmaxf(regressionRise,onsetRise);
    const float r2=total>0?static_cast<float>(1.0-squared/total):0.0f;
    if(!std::isfinite(normalized)||normalized>SLOW_MAX_NORMALIZED_RESIDUAL||r2<SLOW_MIN_R2) { out.reason=ThermalPlantIdReason::PoorFit; return out; }

    const float fitScore=clamp01((r2-SLOW_MIN_R2)/(1.0f-SLOW_MIN_R2));
    const float snrScore=clamp01(observedDelta/fmaxf(0.5f,baselineSigma_*10.0f));
    const float durationScore=clamp01(postSec/(SLOW_MIN_OBSERVATION_SEC*2.0f));
    out.valid=true;out.reason=ThermalPlantIdReason::Valid;out.mode=ThermalPlantModelMode::SlowSlope;
    out.processGain=NAN;out.tauSec=NAN;out.thetaSec=static_cast<float>(theta);out.kPrime=static_cast<float>(kPrime);
    out.residualRmseC=rmse;out.normalizedResidual=normalized;
    out.confidence=0.55f*fitScore+0.25f*snrScore+0.20f*durationScore;
    return out;
  }

  BaselineSample baseline_[BASELINE_WINDOW]{};
  Sample response_[MAX_RESPONSE_SAMPLES]{};
  size_t baselineRingCount_=0,baselineRingNext_=0,responseCount_=0;
  uint32_t baselineTotal_=0,baselineSamplesAtStep_=0,totalSamples_=0;
  bool haveLast_=false,stepDetected_=false,baselineInputStable_=true;
  uint32_t lastTimestampMs_=0,stepTimestampMs_=0;
  float lastActual_=0.0f,deliveredOnSec_=0.0f;
  float baselinePv_=NAN,baselineInput_=NAN,baselineSigma_=NAN;
  float baselineInputMin_=0.0f,baselineInputMax_=0.0f;
  float postInputMin_=0.0f,postInputMax_=0.0f;
  ThermalPlantIdReason terminalReason_=ThermalPlantIdReason::InsufficientSamples;
};
