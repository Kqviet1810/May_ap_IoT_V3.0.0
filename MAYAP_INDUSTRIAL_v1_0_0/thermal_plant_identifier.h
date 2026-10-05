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
  float baselineSlope = NAN;      // degC/s pre-step linear trend diagnostic
  float postSlope = NAN;          // degC/s early post-step trend diagnostic
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
      const bool enoughBaseline = baselineTotal_ >= MIN_BASELINE_SAMPLES;
      const float instantaneousStep = haveLast_ ? actualDeliveredFraction - lastActual_ : 0.0f;
      if (enoughBaseline && haveLast_ && std::fabs(instantaneousStep) >= STEP_DETECT_ABS) {
        freezeBaseline(lastTimestampMs_);
        const float stepFromBaseline = actualDeliveredFraction - baselineInput_;
        if (std::isfinite(stepFromBaseline) && std::fabs(stepFromBaseline) >= MIN_ACTUAL_STEP) {
          stepDetected_ = true;
          // The delivered fraction belongs to the interval ending at this sample,
          // so the physical signed step begins at the previous observation boundary.
          stepTimestampMs_ = lastTimestampMs_;
          deliveredOnSec_ += actualDeliveredFraction * static_cast<float>(timestampMs-lastTimestampMs_) * 0.001f;
          postInputMin_ = postInputMax_ = actualDeliveredFraction;
          appendResponse(timestampMs, filteredPv, actualDeliveredFraction);
        } else {
          pushBaseline(timestampMs, filteredPv, actualDeliveredFraction);
        }
      } else {
        pushBaseline(timestampMs, filteredPv, actualDeliveredFraction);
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
    out.baselineSlope = baselineSlope_;
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
    const float stepMagnitude = std::fabs(actualStep);
    out.actualStep = actualStep;
    if (!std::isfinite(actualStep) || stepMagnitude < MIN_ACTUAL_STEP) { out.reason=ThermalPlantIdReason::NoActualStep; return out; }
    if (!baselineInputStable_) { out.reason=ThermalPlantIdReason::UnstableActualInput; return out; }
    const float inputTolerance=fmaxf(INPUT_STABILITY_ABS,stepMagnitude*INPUT_STABILITY_FRACTION);
    if (std::fabs(postInputMin_-finalInput)>inputTolerance || std::fabs(postInputMax_-finalInput)>inputTolerance) {
      out.reason=ThermalPlantIdReason::UnstableActualInput; return out;
    }

    // V4.3A: non-steady pre-step PV is valid evidence. A noise-aware
    // persistent derivative change gates a continuous piecewise-linear fit.
    // Heater input magnitude/sign still comes only from actual delivered energy.
    if (std::fabs(baselineSlope_) >= TREND_BASELINE_ACTIVE_SLOPE ||
        baselineInput_ > INPUT_OFF_MAX) {
      if (!std::isfinite(baselineTrendRmse_) ||
          baselineTrendRmse_ > TREND_MAX_BASELINE_RMSE_C) {
        out.reason=ThermalPlantIdReason::ExcessiveNoise; return out;
      }
      ThermalPlantModel trend=identifyTrendSlope(out,actualStep,postSec);
      if (trend.valid) return trend;
      if (trend.reason==ThermalPlantIdReason::ExcessiveNoise ||
          trend.reason==ThermalPlantIdReason::PoorFit ||
          trend.reason==ThermalPlantIdReason::NoResponse ||
          trend.reason==ThermalPlantIdReason::InvalidParameters) return trend;
    }

    // Frozen V4.2 settled path below; only sign normalization is added.
    const float y0=baselinePv_;
    const float yFinal=meanResponsePv(finalStart,responseCount_);
    const float delta=yFinal-y0;
    const float responseMagnitude=std::fabs(delta);
    if (!std::isfinite(delta) || responseMagnitude < MIN_RESPONSE_C ||
        delta*actualStep <= 0.0f) { out.reason=ThermalPlantIdReason::NoResponse; return out; }
    if (baselineSigma_ > responseMagnitude*MAX_NOISE_FRACTION) { out.reason=ThermalPlantIdReason::ExcessiveNoise; return out; }

    const uint32_t finalStartMs=response_[finalStart].timestampMs;
    const uint32_t endMs=response_[responseCount_-1].timestampMs;
    const size_t finalHalf=firstAtOrAfter(finalStartMs + (endMs-finalStartMs)/2U);
    if (finalHalf<=finalStart || finalHalf>=responseCount_) return out;
    const float finalDrift=std::fabs(meanResponsePv(finalHalf,responseCount_)-meanResponsePv(finalStart,finalHalf));
    const float settleLimit=fmaxf(MAX_FINAL_DRIFT_C,responseMagnitude*MAX_FINAL_DRIFT_FRACTION);

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
  struct BaselineSample { uint32_t timestampMs; float pv; float actual; };
  struct Sample { uint32_t timestampMs; float pv; float actual; };
  struct Regression {
    bool valid=false;
    float slope=NAN,intercept=NAN,rmse=NAN,slopeStdErr=NAN;
    size_t count=0;
  };

  static constexpr size_t MIN_TOTAL_SAMPLES=40U,MIN_BASELINE_SAMPLES=12U,MIN_RESPONSE_SAMPLES=24U;
  static constexpr float FINAL_WINDOW_MAX_SEC=800.0f,MIN_FINAL_WINDOW_SEC=60.0f;
  static constexpr float INPUT_OFF_MAX=0.02f,MIN_ACTUAL_STEP=0.08f,STEP_DETECT_ABS=0.05f;
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
  // V4.3A trend-only thresholds. The settled V4.2 path above is unchanged.
  static constexpr float TREND_BASELINE_ACTIVE_SLOPE=0.00015f;
  static constexpr float TREND_MAX_BASELINE_RMSE_C=0.12f;
  static constexpr float TREND_MIN_SLOPE_CHANGE=0.00008f;
  static constexpr float TREND_SLOPE_SIGMA_MULT=2.5f;
  static constexpr float TREND_MAX_NORMALIZED_RESIDUAL=0.32f;
  static constexpr float TREND_MIN_SIGNIFICANCE=1.10f;
  static constexpr float TREND_MIN_CONFIDENCE=0.80f;
  static constexpr uint32_t TREND_LOCAL_WINDOW_SEC=180U;
  static constexpr uint32_t TREND_POST_FIT_SEC=600U;
  static constexpr uint8_t TREND_PERSIST_WINDOWS=3U;

  static float clamp01(float value){return value<0?0:value>1?1:value;}

  void pushBaseline(uint32_t timestampMs,float pv,float actual) {
    if (baselineRingCount_ < BASELINE_WINDOW) {
      baseline_[baselineRingCount_++]={timestampMs,pv,actual};
    } else {
      baseline_[baselineRingNext_]={timestampMs,pv,actual};
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

  Regression fitBaseline(uint32_t originMs) const {
    if (baselineRingCount_ < MIN_BASELINE_SAMPLES) return Regression{};
    const double n=static_cast<double>(baselineRingCount_);
    double st=0,sy=0,stt=0,sty=0;
    for(size_t i=0;i<baselineRingCount_;++i) {
      const BaselineSample sample=baselineAt(i);
      const double t=static_cast<int32_t>(sample.timestampMs-originMs)*0.001;
      st+=t; sy+=sample.pv; stt+=t*t; sty+=t*sample.pv;
    }
    const double denom=n*stt-st*st;
    if (!(denom>0.0)) return Regression{};
    const double slope=(n*sty-st*sy)/denom;
    const double intercept=(sy-slope*st)/n;
    double sse=0,sxx=0;
    const double meanT=st/n;
    for(size_t i=0;i<baselineRingCount_;++i) {
      const BaselineSample sample=baselineAt(i);
      const double t=static_cast<int32_t>(sample.timestampMs-originMs)*0.001;
      const double error=sample.pv-(intercept+slope*t);
      const double centered=t-meanT;
      sse+=error*error; sxx+=centered*centered;
    }
    Regression out;
    out.valid=std::isfinite(slope)&&std::isfinite(intercept);
    out.slope=static_cast<float>(slope);
    out.intercept=static_cast<float>(intercept);
    out.rmse=sqrtf(static_cast<float>(sse/n));
    out.slopeStdErr=(n>2.0&&sxx>0.0)
        ? sqrtf(static_cast<float>((sse/(n-2.0))/sxx)) : INFINITY;
    out.count=baselineRingCount_;
    return out;
  }

  void freezeBaseline(uint32_t originMs) {
    baselineSamplesAtStep_=baselineTotal_;
    if (!baselineRingCount_) return;
    double pv=0,input=0;
    for(size_t i=0;i<baselineRingCount_;++i){const BaselineSample sample=baselineAt(i);pv+=sample.pv;input+=sample.actual;}
    baselinePv_=static_cast<float>(pv/baselineRingCount_);
    baselineInput_=static_cast<float>(input/baselineRingCount_);
    double squared=0;
    for(size_t i=0;i<baselineRingCount_;++i){const double d=baselineAt(i).pv-baselinePv_;squared+=d*d;}
    baselineSigma_=sqrtf(static_cast<float>(squared/baselineRingCount_));
    baselineInputStable_=std::fabs(baselineInputMin_-baselineInput_)<=INPUT_STABILITY_ABS &&
                         std::fabs(baselineInputMax_-baselineInput_)<=INPUT_STABILITY_ABS;
    const Regression regression=fitBaseline(originMs);
    if (regression.valid) {
      baselineSlope_=regression.slope;
      baselineTrendAtStep_=regression.intercept;
      baselineTrendRmse_=regression.rmse;
      baselineSlopeStdErr_=regression.slopeStdErr;
    }
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

  Regression fitResponse(size_t first,size_t end) const {
    Regression out;
    if (end<=first+2U || end>responseCount_) return out;
    const double n=static_cast<double>(end-first);
    double st=0,sy=0,stt=0,sty=0;
    for(size_t i=first;i<end;++i) {
      const double t=(response_[i].timestampMs-stepTimestampMs_)*0.001;
      st+=t; sy+=response_[i].pv; stt+=t*t; sty+=t*response_[i].pv;
    }
    const double denom=n*stt-st*st;
    if (!(denom>0.0)) return out;
    const double slope=(n*sty-st*sy)/denom;
    const double intercept=(sy-slope*st)/n;
    double sse=0,sxx=0;
    const double meanT=st/n;
    for(size_t i=first;i<end;++i) {
      const double t=(response_[i].timestampMs-stepTimestampMs_)*0.001;
      const double error=response_[i].pv-(intercept+slope*t);
      const double centered=t-meanT;
      sse+=error*error; sxx+=centered*centered;
    }
    out.valid=std::isfinite(slope)&&std::isfinite(intercept);
    out.slope=static_cast<float>(slope);
    out.intercept=static_cast<float>(intercept);
    out.rmse=sqrtf(static_cast<float>(sse/n));
    out.slopeStdErr=(n>2.0&&sxx>0.0)
        ? sqrtf(static_cast<float>((sse/(n-2.0))/sxx)) : INFINITY;
    out.count=end-first;
    return out;
  }

  ThermalPlantModel identifyTrendSlope(ThermalPlantModel out,float actualStep,float postSec) const {
    if (!std::isfinite(baselineSlope_) || !std::isfinite(baselineTrendAtStep_) ||
        !std::isfinite(baselineSlopeStdErr_)) {
      out.reason=ThermalPlantIdReason::InsufficientSamples; return out;
    }
    const float direction=actualStep>0.0f?1.0f:-1.0f;
    const float slopeThreshold=fmaxf(TREND_MIN_SLOPE_CHANGE,
        TREND_SLOPE_SIGMA_MULT*baselineSlopeStdErr_);

    // Require a persistent derivative change before estimating the change point.
    uint8_t persistent=0;
    size_t firstWindowEnd=responseCount_;
    for(size_t i=0;i<responseCount_;++i) {
      const uint32_t endMs=response_[i].timestampMs+TREND_LOCAL_WINDOW_SEC*1000U;
      size_t end=i;
      while(end<responseCount_ && static_cast<int32_t>(response_[end].timestampMs-endMs)<=0) ++end;
      if (end-i<6U) continue;
      const Regression local=fitResponse(i,end);
      if (!local.valid) continue;
      const float signedChange=(local.slope-baselineSlope_)*direction;
      const float localNoise=fmaxf(slopeThreshold,
          TREND_SLOPE_SIGMA_MULT*local.slopeStdErr);
      if (signedChange>=localNoise) {
        if (!persistent) firstWindowEnd=end;
        if (++persistent>=TREND_PERSIST_WINDOWS) break;
      } else {
        persistent=0;
        firstWindowEnd=responseCount_;
      }
    }
    if (persistent<TREND_PERSIST_WINDOWS || firstWindowEnd==responseCount_) {
      out.reason=ThermalPlantIdReason::NoResponse; return out;
    }

    // Continuous two-line model:
    // pre:  PV=a0+m0*t
    // post: PV=(a0-q*theta)+(m0+q)*t
    // so q=m1-m0 and kPrime=q/dU. Scan theta only after persistent evidence.
    const float horizon=fminf(postSec,static_cast<float>(TREND_POST_FIT_SEC));
    size_t fitEnd=0;
    while(fitEnd<responseCount_ &&
          static_cast<float>(response_[fitEnd].timestampMs-stepTimestampMs_)*0.001f<=horizon) ++fitEnd;
    if (fitEnd<10U) { out.reason=ThermalPlantIdReason::InsufficientSamples; return out; }
    const float detectBound=static_cast<float>(
        response_[firstWindowEnd-1U].timestampMs-stepTimestampMs_)*0.001f;
    double bestSse=INFINITY;
    float bestTheta=NAN,bestQ=NAN;
    size_t bestCount=0;
    for(size_t candidate=0;candidate<fitEnd;++candidate) {
      const float theta=candidate==0 ? 0.0f :
          static_cast<float>(response_[candidate-1U].timestampMs-stepTimestampMs_)*0.001f;
      if (theta>detectBound) break;
      double sxx=0,sxy=0;
      for(size_t i=0;i<fitEnd;++i) {
        const float t=static_cast<float>(response_[i].timestampMs-stepTimestampMs_)*0.001f;
        const float x=fmaxf(0.0f,t-theta);
        const float baseline=baselineTrendAtStep_+baselineSlope_*t;
        const float residual=response_[i].pv-baseline;
        sxx+=static_cast<double>(x)*x;
        sxy+=static_cast<double>(x)*residual;
      }
      if (!(sxx>0.0)) continue;
      const float q=static_cast<float>(sxy/sxx);
      if (!std::isfinite(q) || q*direction<=0.0f) continue;
      double sse=0;
      size_t count=0;
      for(size_t i=0;i<fitEnd;++i) {
        const float t=static_cast<float>(response_[i].timestampMs-stepTimestampMs_)*0.001f;
        const float x=fmaxf(0.0f,t-theta);
        const float predicted=baselineTrendAtStep_+baselineSlope_*t+q*x;
        const float error=response_[i].pv-predicted;
        sse+=static_cast<double>(error)*error;
        ++count;
      }
      if (sse<bestSse) {
        bestSse=sse; bestTheta=theta; bestQ=q; bestCount=count;
      }
    }
    if (!std::isfinite(bestTheta) || !std::isfinite(bestQ) || bestCount<8U) {
      out.reason=ThermalPlantIdReason::InvalidParameters; return out;
    }
    const float signedChange=bestQ*direction;
    if (signedChange<slopeThreshold*TREND_MIN_SIGNIFICANCE) {
      out.reason=ThermalPlantIdReason::NoResponse; return out;
    }
    const float kPrime=bestQ/actualStep;
    if (!std::isfinite(kPrime) || kPrime<=0.0f || kPrime>MAX_PROCESS_GAIN) {
      out.reason=ThermalPlantIdReason::InvalidParameters; return out;
    }
    const float rmse=sqrtf(static_cast<float>(bestSse/bestCount));
    const float signalRise=std::fabs(bestQ)*fmaxf(1.0f,horizon-bestTheta);
    const float normalized=rmse/fmaxf(signalRise,
        fmaxf(0.04f,baselineTrendRmse_*2.0f));
    if (!std::isfinite(normalized) || normalized>TREND_MAX_NORMALIZED_RESIDUAL) {
      out.reason=ThermalPlantIdReason::PoorFit; return out;
    }
    const float significance=clamp01(
        signedChange/fmaxf(slopeThreshold*3.0f,1e-7f));
    const float fitScore=clamp01(
        1.0f-normalized/TREND_MAX_NORMALIZED_RESIDUAL);
    const float durationScore=clamp01(
        postSec/fmaxf(bestTheta+TREND_POST_FIT_SEC,1.0f));
    const float baselineScore=clamp01(
        1.0f-baselineTrendRmse_/fmaxf(signalRise,0.05f));
    const float confidence=0.35f*significance+0.35f*fitScore+
        0.15f*durationScore+0.15f*baselineScore;
    if (confidence<TREND_MIN_CONFIDENCE) {
      out.reason=ThermalPlantIdReason::PoorFit;
      out.confidence=confidence;
      return out;
    }
    out.valid=true;
    out.reason=ThermalPlantIdReason::Valid;
    out.mode=ThermalPlantModelMode::SlowSlope;
    out.processGain=NAN;
    out.tauSec=NAN;
    out.thetaSec=bestTheta;
    out.kPrime=kPrime;
    out.baselineSlope=baselineSlope_;
    out.postSlope=baselineSlope_+bestQ;
    out.residualRmseC=rmse;
    out.normalizedResidual=normalized;
    out.confidence=confidence;
    return out;
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
    const float normalized=rmse/std::fabs(delta);
    if (!std::isfinite(normalized)||normalized>MAX_NORMALIZED_RESIDUAL) { out.reason=ThermalPlantIdReason::PoorFit; return out; }
    const float fitScore=clamp01(1.0f-normalized/MAX_NORMALIZED_RESIDUAL);
    const float durationScore=clamp01(postSec/(theta+4.0f*tau));
    const float stabilityScore=clamp01(1.0f-finalDrift/fmaxf(MAX_FINAL_DRIFT_C,std::fabs(delta)*MAX_FINAL_DRIFT_FRACTION));
    out.valid=true;out.reason=ThermalPlantIdReason::Valid;out.mode=ThermalPlantModelMode::FOPDT;
    out.processGain=gain;out.tauSec=tau;out.thetaSec=theta;out.kPrime=gain/tau;
    out.residualRmseC=rmse;out.normalizedResidual=normalized;
    out.confidence=0.5f*fitScore+0.25f*durationScore+0.25f*stabilityScore;
    return out;
  }

  ThermalPlantModel identifySlowSlope(ThermalPlantModel out,float y0,float observedDelta,float actualStep,float postSec) const {
    if (postSec < SLOW_MIN_OBSERVATION_SEC) return out;
    const float direction=actualStep>=0.0f?1.0f:-1.0f;
    const float stepMagnitude=std::fabs(actualStep);
    const float onsetRise=fmaxf(SLOW_ONSET_MIN_C,baselineSigma_*4.0f);
    size_t onset=responseCount_;
    for(size_t i=1;i+2U<responseCount_;++i) {
      const float z0=(response_[i].pv-y0)*direction;
      const float z1=(response_[i+1U].pv-y0)*direction;
      const float z2=(response_[i+2U].pv-y0)*direction;
      if(z0>=onsetRise && z1>=onsetRise*0.8f && z2>=onsetRise*0.8f){onset=i;break;}
    }
    if(onset==responseCount_) return out;
    const uint32_t regressionEndMs=response_[onset].timestampMs+static_cast<uint32_t>(SLOW_REGRESSION_SEC*1000.0f);
    size_t end=onset;
    while(end<responseCount_ && static_cast<int32_t>(response_[end].timestampMs-regressionEndMs)<=0) ++end;
    if(end-onset<8U) return out;

    double sw=0,st=0,sy=0,stt=0,sty=0;
    for(size_t i=onset;i<end;++i){
      const double t=(response_[i].timestampMs-stepTimestampMs_)*0.001;
      const double z=(response_[i].pv-y0)*direction;
      sw+=1.0;st+=t;sy+=z;stt+=t*t;sty+=t*z;
    }
    const double denom=sw*stt-st*st;
    if(!(denom>0.0)) return out;
    const double slope=(sw*sty-st*sy)/denom;
    const double intercept=(sy-slope*st)/sw;
    if(!std::isfinite(slope)||slope<=0.0) { out.reason=ThermalPlantIdReason::InvalidParameters; return out; }
    const double theta=-intercept/slope;
    const double kPrime=slope/stepMagnitude;
    if(!std::isfinite(theta)||!std::isfinite(kPrime)||theta<0.0||theta>MAX_THETA_SEC||kPrime<=0.0||kPrime>MAX_PROCESS_GAIN) {
      out.reason=ThermalPlantIdReason::InvalidParameters; return out;
    }

    double squared=0,total=0,mean=sy/sw;
    for(size_t i=onset;i<end;++i){
      const double t=(response_[i].timestampMs-stepTimestampMs_)*0.001;
      const double z=(response_[i].pv-y0)*direction;
      const double predicted=intercept+slope*t;
      const double e=z-predicted;
      const double d=z-mean;
      squared+=e*e;total+=d*d;
    }
    const float rmse=sqrtf(static_cast<float>(squared/sw));
    const float regressionRise=static_cast<float>(slope*((response_[end-1U].timestampMs-response_[onset].timestampMs)*0.001));
    const float normalized=rmse/fmaxf(regressionRise,onsetRise);
    const float r2=total>0?static_cast<float>(1.0-squared/total):0.0f;
    if(!std::isfinite(normalized)||normalized>SLOW_MAX_NORMALIZED_RESIDUAL||r2<SLOW_MIN_R2) { out.reason=ThermalPlantIdReason::PoorFit; return out; }

    const float fitScore=clamp01((r2-SLOW_MIN_R2)/(1.0f-SLOW_MIN_R2));
    const float snrScore=clamp01(std::fabs(observedDelta)/fmaxf(0.5f,baselineSigma_*10.0f));
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
  float baselineSlope_=0.0f,baselineTrendAtStep_=NAN;
  float baselineTrendRmse_=NAN,baselineSlopeStdErr_=NAN;
  ThermalPlantIdReason terminalReason_=ThermalPlantIdReason::InsufficientSamples;
};
