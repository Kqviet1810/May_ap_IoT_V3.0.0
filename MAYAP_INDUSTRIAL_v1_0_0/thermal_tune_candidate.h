#pragma once

#include "thermal_plant_identifier.h"
#include <cmath>
#include <cstdint>

enum class ThermalTuneCandidateReason : uint8_t {
  Valid,
  InvalidPlantModel,
  InvalidTauC,
  TauCBelowTheta,
  InvalidResult
};

inline const char *thermalTuneCandidateReasonName(ThermalTuneCandidateReason reason) {
  switch(reason) {
    case ThermalTuneCandidateReason::Valid:return "VALID";
    case ThermalTuneCandidateReason::InvalidPlantModel:return "INVALID_PLANT_MODEL";
    case ThermalTuneCandidateReason::InvalidTauC:return "INVALID_TAUC";
    case ThermalTuneCandidateReason::TauCBelowTheta:return "TAUC_BELOW_THETA";
    case ThermalTuneCandidateReason::InvalidResult:return "INVALID_RESULT";
  }
  return "UNKNOWN";
}

struct ThermalTuneCandidate {
  bool valid=false;
  ThermalTuneCandidateReason reason=ThermalTuneCandidateReason::InvalidPlantModel;
  ThermalPlantModelMode modelMode=ThermalPlantModelMode::None;
  float tauC=NAN;
  float kcFractionPerC=NAN; // mathematical SIMC gain before runtime unit conversion
  float Kp=NAN;             // runtime output-percent / degC
  float Ki=NAN;             // runtime output-percent / (degC*s)
  float Kd=0.0f;            // V4 is PI only
  float Ti=NAN;
};

inline ThermalTuneCandidate generateThermalSimcPiCandidate(const ThermalPlantModel &plant,float tauC) {
  ThermalTuneCandidate out;out.modelMode=plant.mode;out.tauC=tauC;out.Kd=0.0f;
  if(!plant.valid || plant.reason!=ThermalPlantIdReason::Valid || !std::isfinite(plant.thetaSec) || plant.thetaSec<0.0f) return out;
  if(!std::isfinite(tauC) || tauC<=0.0f) {out.reason=ThermalTuneCandidateReason::InvalidTauC;return out;}
  if(tauC<plant.thetaSec) {out.reason=ThermalTuneCandidateReason::TauCBelowTheta;return out;}
  const float lambda=tauC+plant.thetaSec;
  float kc=NAN,ti=NAN;
  if(plant.mode==ThermalPlantModelMode::FOPDT) {
    if(!std::isfinite(plant.processGain)||!std::isfinite(plant.tauSec)||plant.processGain<=0.0f||plant.tauSec<=0.0f) return out;
    kc=(plant.tauSec/plant.processGain)/lambda;
    ti=fminf(plant.tauSec,4.0f*lambda);
  } else if(plant.mode==ThermalPlantModelMode::SlowSlope) {
    if(!std::isfinite(plant.kPrime)||plant.kPrime<=0.0f) return out;
    kc=1.0f/(plant.kPrime*lambda);
    ti=4.0f*lambda;
  } else return out;
  const float kpRuntime=100.0f*kc;
  const float kiRuntime=kpRuntime/ti;
  if(!std::isfinite(kc)||!std::isfinite(ti)||!std::isfinite(kpRuntime)||!std::isfinite(kiRuntime)||kc<=0.0f||ti<=0.0f||kpRuntime<=0.0f||kiRuntime<=0.0f) {
    out.reason=ThermalTuneCandidateReason::InvalidResult;return out;
  }
  out.valid=true;out.reason=ThermalTuneCandidateReason::Valid;out.kcFractionPerC=kc;
  out.Kp=kpRuntime;out.Ki=kiRuntime;out.Kd=0.0f;out.Ti=ti;return out;
}
