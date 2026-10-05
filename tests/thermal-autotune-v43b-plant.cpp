// V4.3B production AutoTune subset. Uses the real start/update/heating bodies
// extracted by tools/test_thermal_control.py and the frozen uncalibrated plants.
// This file deliberately contains NO alternate tuning coordinator.
#include "thermal-autotune-harness.h"
#include "actual-filter.inc"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

struct Plant { const char *name; double capacity,loss; };
struct Model {
  Plant plant;
  double ambient,temp,watts=0.0;
  std::vector<double> delay;
  size_t cursor=0;
  Model(Plant p,double a,unsigned dead,double initial)
      : plant(p),ambient(a),temp(initial),delay(std::max(1U,dead*10U),0.0) {}
  void step(bool on) {
    const double delivered=on?16000.0:0.0;
    const double delayed=delay[cursor];
    delay[cursor]=delivered;
    cursor=(cursor+1U)%delay.size();
    watts+=(delayed-watts)*0.1/8.0;
    temp+=(watts-plant.loss*(temp-ambient))*0.1/plant.capacity;
    assert(std::isfinite(temp));
  }
};

struct PostMetrics {
  double ripple=0.0;
  int settling=-1;
  bool high=false,emergency=false;
};

static PostMetrics postCandidate(Plant plant,double ambient,unsigned dead,
                                 double resolution,const MachineConfig &cfg) {
  Model model(plant,ambient,dead,ambient);
  ThermalController pid;
  HeaterBurstScheduler burst(1U,HEATER_BURST_QUANTUM_MS);
  ActualSensorFilter filter;
  double power=0.0;
  std::vector<double> tail;
  int lastOutside=0;
  PostMetrics out;
  for(unsigned tick=0;tick<108000U;++tick) {
    const uint32_t ms=1000U+tick*100U;
    const double sec=tick*0.1;
    if(tick%20U==0U) {
      filter.updateFilter(static_cast<float>(
          std::round(model.temp/resolution)*resolution),60.0f);
      if(sec>=12.0)
        power=pid.updateOnNewSample(ms,cfg.targetTemp,filter.value(),cfg,true);
    }
    const bool on=burst.update(ms,static_cast<float>(power),sec>=12.0).groupA;
    model.step(on);
    const double error=model.temp-cfg.targetTemp;
    if(std::fabs(error)>0.15)lastOutside=static_cast<int>(sec);
    out.high=out.high||model.temp>=cfg.highTempAlarm;
    out.emergency=out.emergency||model.temp>=cfg.emergencyTemp;
    if(sec>=9000.0 && tick%10U==0U)tail.push_back(model.temp);
  }
  out.settling=lastOutside<9000?lastOutside+1:-1;
  if(!tail.empty())
    out.ripple=*std::max_element(tail.begin(),tail.end())-
               *std::min_element(tail.begin(),tail.end());
  return out;
}

struct ResultRow {
  std::string plant,start,terminal,direction,mode;
  unsigned dead=0;
  double resolution=0,u0=NAN,baselineSlope=NAN,du=NAN,theta=NAN,kPrime=NAN;
  double confidence=NAN,tauC=NAN,kp=NAN,ki=NAN,kd=NAN,ti=NAN;
  bool captureTimeout=false,baselineTimeout=false,modelValid=false;
  bool candidate=false,validation=false,accepted=false,rejected=false,bad=false;
  bool highId=false,emergencyId=false,highVal=false,emergencyVal=false;
  unsigned saves=0;
};

static ResultRow runCase(Plant plant,unsigned dead,double resolution,bool cold) {
  const double ambient=25.0;
  const double initial=cold?ambient:37.0;
  TuneHarness h;
  Model model(plant,ambient,dead,initial);
  ActualSensorFilter filter;
  filter.updateFilter(static_cast<float>(initial),60.0f);
  h.sample(static_cast<float>(initial),filter.value());

  const char *message=nullptr;
  assert(h.startAutoTune(1000U,message));
  ResultRow row;
  row.plant=plant.name;
  row.dead=dead;
  row.resolution=resolution;
  row.start=cold?"COLD":"NEAR_SP";

  AutoTunePhase previous=h.autotune_.phase();
  for(unsigned tick=0;tick<=80UL*60UL*1000UL/100U+1U;++tick) {
    const uint32_t ms=1000U+tick*100U;
    const bool newSample=tick%20U==0U;
    if(newSample) {
      const float raw=static_cast<float>(
          std::round(model.temp/resolution)*resolution);
      filter.updateFilter(raw,60.0f);
      h.sample(raw,filter.value());
      // Mirror production safety inputs; updateAutoTune also hard-checks raw PV.
      h.highTemperatureActive_=model.temp>=h.config_.highTempAlarm;
      h.emergencyActive_=model.temp>=h.config_.emergencyTemp;
    }

    h.cycle(ms,newSample);
    const AutoTunePhase phase=h.autotune_.phase();
    if(phase==AutoTunePhase::ManualBaseline ||
       phase==AutoTunePhase::IdentifyStep) {
      row.highId=row.highId||model.temp>=h.config_.highTempAlarm;
      row.emergencyId=row.emergencyId||model.temp>=h.config_.emergencyTemp;
    }
    if(phase==AutoTunePhase::Validating) {
      row.validation=true;
      row.highVal=row.highVal||model.temp>=h.config_.highTempAlarm;
      row.emergencyVal=row.emergencyVal||model.temp>=h.config_.emergencyTemp;
    }
    model.step(h.outputs_.state().heaterSsr);
    row.highId=row.highId||
        ((phase==AutoTunePhase::ManualBaseline ||
          phase==AutoTunePhase::IdentifyStep) &&
         model.temp>=h.config_.highTempAlarm);
    row.emergencyId=row.emergencyId||
        ((phase==AutoTunePhase::ManualBaseline ||
          phase==AutoTunePhase::IdentifyStep) &&
         model.temp>=h.config_.emergencyTemp);
    row.highVal=row.highVal||
        (phase==AutoTunePhase::Validating &&
         model.temp>=h.config_.highTempAlarm);
    row.emergencyVal=row.emergencyVal||
        (phase==AutoTunePhase::Validating &&
         model.temp>=h.config_.emergencyTemp);

    if(previous!=phase && phase==AutoTunePhase::Validating)
      row.validation=true;
    previous=phase;
    if(!h.autotune_.running())break;
  }

  const auto &m=h.autotune_.model();
  const auto &r=h.autotune_.result();
  row.u0=h.autotune_.baselineFraction();
  row.baselineSlope=m.baselineSlope;
  row.du=m.actualStep;
  row.theta=m.thetaSec;
  row.kPrime=m.kPrime;
  row.confidence=m.confidence;
  row.tauC=r.tauC;
  row.modelValid=m.valid;
  row.mode=thermalPlantModelModeName(m.mode);
  row.direction=h.autotune_.stepDirection()<0?"NEG":
      h.autotune_.stepDirection()>0?"POS":"NONE";
  row.candidate=h.autotune_.candidateGenerated();
  if(row.candidate) {
    row.kp=h.autotune_.candidate().kp;
    row.ki=h.autotune_.candidate().ki;
    row.kd=h.autotune_.candidate().kd;
    row.ti=r.Ti;
  }
  row.accepted=h.autotune_.state()==AutoTuneState::Success &&
               h.autotune_.phase()==AutoTunePhase::Accepted;
  row.rejected=!row.accepted;
  row.terminal=autoTuneReasonName(h.autotune_.reason());
  row.captureTimeout=h.autotune_.reason()==AutoTuneReason::CaptureTimeout;
  row.baselineTimeout=h.autotune_.reason()==AutoTuneReason::BaselineTimeout;
  row.saves=h.store_.saves;

  MachineConfig stored{};
  assert(h.store_.loadConfig(stored));
  if(row.accepted) {
    assert(row.saves==1U);
    assert(stored.kp==h.config_.kp && stored.ki==h.config_.ki &&
           stored.kd==h.config_.kd);
    assert(h.config_.kd==0.0f);
    const PostMetrics post=postCandidate(plant,ambient,dead,resolution,h.config_);
    row.bad=post.emergency||post.settling<0||post.ripple>1.0;
  } else {
    assert(row.saves==0U);
    assert(h.config_.kp==18.0f && h.config_.ki==0.8f && h.config_.kd==45.0f);
    assert(stored.kp==18.0f && stored.ki==0.8f && stored.kd==45.0f);
  }
  return row;
}

int main() {
  const Plant plants[]={
    {"light",180000,120},
    {"medium",600000,180},
    {"heavy",1600000,300}
  };
  std::vector<ResultRow> rows;
  for(const auto &plant:plants) {
    rows.push_back(runCase(plant,5U,0.1,true));
    rows.push_back(runCase(plant,5U,0.01,false));
    rows.push_back(runCase(plant,60U,0.1,false));
    rows.push_back(runCase(plant,60U,0.01,true));
  }

  std::cout<<std::fixed<<std::setprecision(7);
  std::cout<<"plant,deadtime,sensor_resolution,start_condition,u0,baselineSlope,"
              "step_direction,actual_du,model,theta,kPrime,confidence,tauC,"
              "Kp,Ki,Kd,Ti,model_valid,candidate_generated,validation_started,"
              "accepted,rejected,accepted_bad,high_identification,"
              "emergency_identification,high_validation,emergency_validation,"
              "capture_timeout,manual_baseline_timeout,eeprom_saves,terminal_reason\n";
  for(const auto &r:rows) {
    std::cout<<r.plant<<','<<r.dead<<','<<r.resolution<<','<<r.start<<','
      <<r.u0<<','<<r.baselineSlope<<','<<r.direction<<','<<r.du<<','
      <<r.mode<<','<<r.theta<<','<<r.kPrime<<','<<r.confidence<<','
      <<r.tauC<<','<<r.kp<<','<<r.ki<<','<<r.kd<<','<<r.ti<<','
      <<r.modelValid<<','<<r.candidate<<','<<r.validation<<','
      <<r.accepted<<','<<r.rejected<<','<<r.bad<<','
      <<r.highId<<','<<r.emergencyId<<','<<r.highVal<<','<<r.emergencyVal<<','
      <<r.captureTimeout<<','<<r.baselineTimeout<<','<<r.saves<<','
      <<r.terminal<<'\n';
  }

  unsigned valid=0,validation=0,accepted=0,bad=0,highId=0;
  unsigned emergencyId=0,highVal=0,emergencyVal=0;
  unsigned acceptLight=0,acceptMedium=0,acceptHeavy=0;
  for(const auto &r:rows) {
    valid+=r.modelValid;
    validation+=r.validation;
    accepted+=r.accepted;
    bad+=r.bad;
    highId+=r.highId;
    emergencyId+=r.emergencyId;
    highVal+=r.highVal;
    emergencyVal+=r.emergencyVal;
    if(r.accepted && r.plant=="light")++acceptLight;
    if(r.accepted && r.plant=="medium")++acceptMedium;
    if(r.accepted && r.plant=="heavy")++acceptHeavy;
  }
  std::cerr<<"FUNNEL models="<<valid<<"/12 validation="<<validation
           <<"/12 accepted="<<accepted<<"/12 accepted_bad="<<bad
           <<" high_id="<<highId<<" emergency_id="<<emergencyId
           <<" high_val="<<highVal<<" emergency_val="<<emergencyVal
           <<" accept_by_plant="<<acceptLight<<"/"<<acceptMedium<<"/"<<acceptHeavy<<"\n";
  assert(rows.size()==12U);
  assert(valid>=10U);
  assert(validation>=8U);
  assert(accepted>=6U);
  assert(acceptLight>=1U && acceptMedium>=1U && acceptHeavy>=1U);
  assert(bad==0U);
  assert(highId==0U);
  assert(emergencyId==0U && emergencyVal==0U);
}
