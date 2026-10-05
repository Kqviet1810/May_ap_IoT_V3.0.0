#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

// Offline/commissioning-only FOPDT identification. This class observes actual
// delivered heater state; it has no actuator, configuration or persistence API.
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

struct ThermalPlantModel {
  bool valid = false;
  ThermalPlantIdReason reason = ThermalPlantIdReason::InsufficientSamples;
  float processGain = NAN;       // degC per unit actual delivered heater input
  float tauSec = NAN;
  float thetaSec = NAN;
  float residualRmseC = NAN;
  float normalizedResidual = NAN;
  float confidence = 0.0f;
  float actualStep = NAN;
  float deliveredOnSec = 0.0f;
  uint16_t samples = 0;
};

class ThermalPlantIdentifier {
 public:
  // 8192 samples cover more than 11 hours at a 5-second commissioning cadence.
  static constexpr size_t MAX_SAMPLES = 8192U;

  void reset() {
    count_ = 0; terminalReason_ = ThermalPlantIdReason::InsufficientSamples;
  }

  // actualDeliveredFraction must describe measured/known SSR delivery during
  // the interval (0/1 for an SSR), never controller-requested duty.
  bool addSample(uint32_t timestampMs, float filteredPv, float actualDeliveredFraction) {
    if (!std::isfinite(filteredPv) || !std::isfinite(actualDeliveredFraction) ||
        actualDeliveredFraction < 0.0f || actualDeliveredFraction > 1.0f) {
      terminalReason_ = ThermalPlantIdReason::InvalidSample; return false;
    }
    if (count_ && static_cast<int32_t>(timestampMs - samples_[count_-1].timestampMs) <= 0) {
      terminalReason_ = ThermalPlantIdReason::NonMonotonicTime; return false;
    }
    if (count_ >= MAX_SAMPLES) {
      terminalReason_ = ThermalPlantIdReason::CapacityExceeded; return false;
    }
    samples_[count_++] = {timestampMs, filteredPv, actualDeliveredFraction};
    return true;
  }

  ThermalPlantModel identify() const {
    ThermalPlantModel out; out.samples = static_cast<uint16_t>(count_);
    if (terminalReason_ == ThermalPlantIdReason::InvalidSample ||
        terminalReason_ == ThermalPlantIdReason::NonMonotonicTime ||
        terminalReason_ == ThermalPlantIdReason::CapacityExceeded) {
      out.reason = terminalReason_; return out;
    }
    if (count_ < MIN_TOTAL_SAMPLES) return out;

    size_t step = count_;
    for (size_t i=1;i<count_;++i) {
      if (samples_[i-1].actual <= INPUT_OFF_MAX && samples_[i].actual > INPUT_OFF_MAX) {
        step=i; break;
      }
    }
    if (step < MIN_BASELINE_SAMPLES || step == count_ ||
        count_-step < MIN_RESPONSE_SAMPLES) {
      out.reason = step==count_ ? ThermalPlantIdReason::NoActualStep
                                : ThermalPlantIdReason::InsufficientSamples;
      return out;
    }

    const size_t baseStart = step > BASELINE_WINDOW ? step-BASELINE_WINDOW : 0;
    const float baselineInput = meanInput(baseStart, step);
    const size_t tailCount = minSize(FINAL_WINDOW_MAX, (count_-step)/5U);
    if (tailCount < MIN_FINAL_SAMPLES) { out.reason=ThermalPlantIdReason::InsufficientSamples; return out; }
    const size_t finalStart = count_-tailCount;
    const float finalInput = meanInput(finalStart,count_);
    const float actualStep = finalInput-baselineInput;
    if (!std::isfinite(actualStep) || actualStep < MIN_ACTUAL_STEP) {
      out.reason=ThermalPlantIdReason::NoActualStep; return out;
    }
    for (size_t i=0;i<step;++i) if (std::fabs(samples_[i].actual-baselineInput)>INPUT_STABILITY_ABS) {
      out.reason=ThermalPlantIdReason::UnstableActualInput; return out;
    }
    const float inputTolerance=fmaxf(INPUT_STABILITY_ABS,actualStep*INPUT_STABILITY_FRACTION);
    for (size_t i=step;i<count_;++i) if (std::fabs(samples_[i].actual-finalInput)>inputTolerance) {
      out.reason=ThermalPlantIdReason::UnstableActualInput; return out;
    }

    const float y0=meanPv(baseStart,step),yFinal=meanPv(finalStart,count_);
    const float delta=yFinal-y0;
    const float baseSigma=stddevPv(baseStart,step,y0);
    if (!std::isfinite(delta) || delta < MIN_RESPONSE_C) {
      out.reason=ThermalPlantIdReason::NoResponse; return out;
    }
    if (baseSigma > delta*MAX_NOISE_FRACTION) {
      out.reason=ThermalPlantIdReason::ExcessiveNoise; return out;
    }
    const size_t half=tailCount/2U;
    const float finalDrift=std::fabs(meanPv(count_-half,count_)-meanPv(finalStart,count_-half));
    if (finalDrift > fmaxf(MAX_FINAL_DRIFT_C,delta*MAX_FINAL_DRIFT_FRACTION)) {
      out.reason=ThermalPlantIdReason::NotSettled; return out;
    }

    float t28=NAN,t63=NAN;
    if (!crossingTime(step,y0,delta,0.283f,t28) || !crossingTime(step,y0,delta,0.632f,t63)) {
      out.reason=ThermalPlantIdReason::MissingCrossing; return out;
    }
    const float tau=1.5f*(t63-t28);
    const float theta=fmaxf(0.0f,1.5f*t28-0.5f*t63);
    const float gain=delta/actualStep;
    if (!std::isfinite(gain)||!std::isfinite(tau)||!std::isfinite(theta)||
        gain<=0.0f||gain>MAX_PROCESS_GAIN||tau<MIN_TAU_SEC||tau>MAX_TAU_SEC||theta>MAX_THETA_SEC) {
      out.reason=ThermalPlantIdReason::InvalidParameters; return out;
    }

    double squared=0.0;size_t fitSamples=0;float delivered=0.0f;
    const uint32_t stepAt=samples_[step].timestampMs;
    for(size_t i=step;i<count_;++i) {
      const float t=(samples_[i].timestampMs-stepAt)*0.001f;
      const float predicted=t<=theta?y0:y0+gain*actualStep*(1.0f-expf(-(t-theta)/tau));
      const float error=samples_[i].pv-predicted;squared+=error*error;++fitSamples;
      if(i>step)delivered+=samples_[i-1].actual*(samples_[i].timestampMs-samples_[i-1].timestampMs)*0.001f;
    }
    const float rmse=fitSamples?sqrtf(static_cast<float>(squared/fitSamples)):INFINITY;
    const float normalized=rmse/delta;
    if (!std::isfinite(normalized)||normalized>MAX_NORMALIZED_RESIDUAL) {
      out.reason=ThermalPlantIdReason::PoorFit; return out;
    }
    const float postSec=(samples_[count_-1].timestampMs-stepAt)*0.001f;
    const float fitScore=clamp01(1.0f-normalized/MAX_NORMALIZED_RESIDUAL);
    const float durationScore=clamp01(postSec/(theta+4.0f*tau));
    const float stabilityScore=clamp01(1.0f-finalDrift/fmaxf(MAX_FINAL_DRIFT_C,delta*MAX_FINAL_DRIFT_FRACTION));
    out.valid=true;out.reason=ThermalPlantIdReason::Valid;out.processGain=gain;
    out.tauSec=tau;out.thetaSec=theta;out.residualRmseC=rmse;
    out.normalizedResidual=normalized;out.confidence=0.5f*fitScore+0.25f*durationScore+0.25f*stabilityScore;
    out.actualStep=actualStep;out.deliveredOnSec=delivered;return out;
  }

 private:
  struct Sample { uint32_t timestampMs; float pv; float actual; };
  static constexpr size_t MIN_TOTAL_SAMPLES=40U,MIN_BASELINE_SAMPLES=12U,MIN_RESPONSE_SAMPLES=24U;
  static constexpr size_t BASELINE_WINDOW=60U,FINAL_WINDOW_MAX=160U,MIN_FINAL_SAMPLES=12U;
  static constexpr float INPUT_OFF_MAX=0.02f,MIN_ACTUAL_STEP=0.10f;
  static constexpr float INPUT_STABILITY_ABS=0.02f,INPUT_STABILITY_FRACTION=0.05f;
  static constexpr float MIN_RESPONSE_C=0.25f,MAX_NOISE_FRACTION=0.20f;
  static constexpr float MAX_FINAL_DRIFT_C=0.05f,MAX_FINAL_DRIFT_FRACTION=0.03f;
  static constexpr float MAX_NORMALIZED_RESIDUAL=0.15f;
  static constexpr float MIN_TAU_SEC=1.0f,MAX_TAU_SEC=86400.0f,MAX_THETA_SEC=21600.0f;
  static constexpr float MAX_PROCESS_GAIN=1000.0f;

  static size_t minSize(size_t a,size_t b){return a<b?a:b;}
  static float clamp01(float value){return value<0?0:value>1?1:value;}
  float meanPv(size_t first,size_t end)const{double sum=0;for(size_t i=first;i<end;++i)sum+=samples_[i].pv;return static_cast<float>(sum/(end-first));}
  float meanInput(size_t first,size_t end)const{double sum=0;for(size_t i=first;i<end;++i)sum+=samples_[i].actual;return static_cast<float>(sum/(end-first));}
  float stddevPv(size_t first,size_t end,float mean)const{double sum=0;for(size_t i=first;i<end;++i){const double d=samples_[i].pv-mean;sum+=d*d;}return sqrtf(static_cast<float>(sum/(end-first)));}
  bool crossingTime(size_t step,float y0,float delta,float level,float &seconds)const{
    const uint32_t stepAt=samples_[step].timestampMs;
    for(size_t i=step+1;i+2<count_;++i){
      const float normalized=(samples_[i].pv-y0)/delta;
      if(normalized<level||(samples_[i+1].pv-y0)/delta<level-0.02f||
         (samples_[i+2].pv-y0)/delta<level-0.02f)continue;
      const float previous=(samples_[i-1].pv-y0)/delta;
      const float fraction=normalized>previous?(level-previous)/(normalized-previous):0.0f;
      const float at=samples_[i-1].timestampMs+
          clamp01(fraction)*static_cast<float>(samples_[i].timestampMs-samples_[i-1].timestampMs);
      seconds=(at-stepAt)*0.001f;return true;
    }
    return false;
  }

  Sample samples_[MAX_SAMPLES]{};
  size_t count_=0;
  ThermalPlantIdReason terminalReason_=ThermalPlantIdReason::InsufficientSamples;
};
