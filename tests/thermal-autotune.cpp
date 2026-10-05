#include "thermal-autotune-harness.h"

static void assertOld(const TuneHarness &h) {
  assert(h.config_.kp==18.0f);
  assert(h.config_.ki==0.8f);
  assert(h.config_.kd==45.0f);
}

static void startCancelAndFaultKeepLkg() {
  {
    TuneHarness h;
    h.sample(37.0f,37.0f);
    const char *message=nullptr;
    assert(h.startAutoTune(1000U,message));
    h.cycle(1000U);
    assert(h.autotune_.phase()==AutoTunePhase::CapturePower);
    assert(h.store_.saves==0U);
    assertOld(h);
    h.autotune_.cancel();
    h.heaterBurst_.reset();
    assert(h.autotune_.state()==AutoTuneState::Idle);
    assert(h.store_.saves==0U);
    assertOld(h);
  }
  {
    TuneHarness h;
    h.sample(37.0f,37.0f);
    const char *message=nullptr;
    assert(h.startAutoTune(1000U,message));
    h.cycle(1000U);
    h.faults_.inhibit=true;
    h.cycle(1100U,false);
    assert(h.autotune_.state()==AutoTuneState::Failed);
    assert(h.autotune_.reason()==AutoTuneReason::SafetyAbort);
    assert(!h.outputs_.state().heaterSsr);
    assert(h.store_.saves==0U);
    assertOld(h);
  }
}

// Drive the production RelayAutoTune with actual 0/1 PDM delivery. CAPTURE gets
// a dithered LKG-like 20% actual source; once MANUAL_BASELINE owns heat, its
// own fixed command goes through the same HeaterBurstScheduler. The synthetic
// PV deliberately has a non-zero pre-step slope.
static void driftingNegativeStepReachesValidation() {
  MachineConfig cfg;
  RelayAutoTune tune;
  tune.configure(cfg.targetTemp);
  uint32_t now=1000U;
  float pv=37.0f;
  tune.start(now,pv);
  MachineConfig tuned{};
  assert(!tune.update(now,pv,cfg,tuned));
  assert(tune.phase()==AutoTunePhase::CapturePower);

  HeaterBurstScheduler burst(1U,HEATER_BURST_QUANTUM_MS);
  bool actualOn=false;
  uint32_t manualAt=0U,stepAt=0U,candidateAt=0U;
  float manualPv=37.0f;
  constexpr float baselineSlope=0.0015f;
  constexpr float kPrime=0.0200f;
  constexpr float thetaSec=30.0f;

  for(unsigned n=0;n<24000U && !tune.candidateGenerated();++n) {
    now+=100U;
    float commanded=20.0f;
    if(tune.phase()==AutoTunePhase::ManualBaseline ||
       tune.phase()==AutoTunePhase::IdentifyStep ||
       tune.phase()==AutoTunePhase::ModelReady)
      commanded=tune.power();
    actualOn=burst.update(now,commanded,true).groupA;

    if(tune.phase()==AutoTunePhase::ManualBaseline) {
      if(manualAt==0U){manualAt=now;manualPv=pv;}
      const float t=(now-manualAt)*0.001f;
      pv=manualPv+baselineSlope*t;
    } else if(tune.phase()==AutoTunePhase::IdentifyStep) {
      if(stepAt==0U)stepAt=now;
      const float totalT=(now-manualAt)*0.001f;
      const float stepT=(now-stepAt)*0.001f;
      const float du=-0.10f;
      pv=manualPv+baselineSlope*totalT+
          kPrime*du*fmaxf(0.0f,stepT-thetaSec);
    }

    tune.observeActual(now,actualOn,pv);
    if((now-1000U)%2000U==0U) {
      const bool ready=tune.update(now,pv,cfg,tuned);
      if(tune.phase()==AutoTunePhase::ManualBaseline && manualAt==0U) {
        manualAt=now;manualPv=pv;
      }
      if(tune.phase()==AutoTunePhase::IdentifyStep && stepAt==0U)
        stepAt=now;
      if(ready)candidateAt=now;
    }
  }

  assert(tune.candidateGenerated());
  assert(candidateAt!=0U);
  assert(tune.model().valid);
  assert(tune.model().mode==ThermalPlantModelMode::SlowSlope);
  assert(std::isfinite(tune.model().kPrime) && tune.model().kPrime>0.0f);
  assert(tune.model().actualStep<0.0f);
  assert(tune.stepDirection()<0);
  assert(tuned.kd==0.0f);
  assert(tuned.kp<=cfg.kp+0.0005f);
  assert(tune.result().tauC>=tune.model().thetaSec);
  assert(tune.result().Ti>0.0f);

  tune.beginValidation(now,37.5f);
  assert(tune.phase()==AutoTunePhase::ValidationSettle);
  pv=37.5f;
  for(unsigned n=0;n<1000U && tune.running();++n) {
    now+=2000U;
    tune.observeActual(now,false,pv);
    tune.update(now,pv,cfg,tuned);
    if(tune.validating())
      tune.validate(now,pv,20.0f,20.0f,cfg.maxHeaterPower,false);
    if(tune.state()==AutoTuneState::Success)break;
  }
  assert(tune.state()==AutoTuneState::Success);
  assert(tune.phase()==AutoTunePhase::Accepted);
  assert(tune.candidate().kd==0.0f);
}

int main() {
  startCancelAndFaultKeepLkg();
  driftingNegativeStepReachesValidation();
  std::puts("AutoTune V4.3B targeted: actual-energy capture, drifting negative step, SIMC PI, settle/validation, cancel/fault LKG retention PASS");
}
