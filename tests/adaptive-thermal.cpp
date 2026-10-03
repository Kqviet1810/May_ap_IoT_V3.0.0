#include "../MAYAP_INDUSTRIAL_v1_0_0/adaptive_thermal_balance.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/adaptive_persistence.h"
#define MAYAP_TEST_ADAPTIVE 1
#define MAYAP_SENSOR_PROFILE 0
#define MAYAP_ADAPTIVE_OBSERVER_ONLY 0
#include "thermal-autotune-harness.h"
#include "actual-filter.inc"
#include <ctime>
using namespace MayapAdaptive;
struct Baseline: TuneHarness {
#include "fixtures/thermal-v2-baseline/heating.inc"
};
static Observation input(){Observation o;o.sensor=o.fanStable=true;o.pv=o.raw=o.sp=37.5f;o.high=38.2f;return o;}
static void qualify(AdaptiveThermalSupervisor &s,uint32_t start=0){
  auto o=input();
  for(uint32_t t=0;t<1500000;t+=5){s.tick(start+t,t%1000<300);if(t%2000==0)s.sample(start+t,o);s.update(start+t,true,100,o);}
}
static void rollback(){
  for(float sp:{30.0f,32.0f,35.0f,37.5f}){
    TuneHarness actual;Baseline old;actual.batchRunning_=old.batchRunning_=true;
    actual.config_.targetTemp=old.config_.targetTemp=sp;
    for(uint32_t t=1000;t<361000;t+=5){
      const float pv=sp-1+std::sin(t*.00005f)*.5f;
      actual.sample(pv,pv);old.sample(pv,pv);actual.cycle(t,t%2000==0);
      old.newSensorSample_=t%2000==0;old.updateHeatingAndOutputs(t);
      assert(actual.outputs_.state().heaterSsr==old.outputs_.state().heaterSsr);
      assert(actual.outputs_.state().ventFan==old.outputs_.state().ventFan);
      assert(actual.runtime_.heaterPower==old.runtime_.heaterPower);
    }
  }
  AdaptiveThermalSupervisor s;qualify(s);assert(s.decision().effective<100);
  auto o=input();o.pv=o.raw=37.8f;
  for(uint32_t t=1500000;t<1800000;t+=5){s.tick(t,false);if(t%2000==0){o.pv+=.0008f;o.raw=o.pv;s.sample(t,o);}s.update(t,true,100,o);}
  auto off=s.update(1800001,false,50,o);assert(off.state==State::Disabled&&off.effective==50&&!off.cooling);
  auto on=s.update(1800006,true,50,input());assert(on.state==State::Learning&&on.effective==50);
  assert(s.observer().estimates().confidence==0);
}
static void envelopes(){
  AdaptiveThermalSupervisor s;qualify(s);auto o=input();
  float last=s.decision().effective;
  for(uint32_t t=1500000;t<1800000;t+=5){
    s.tick(t,t%1000<300);if(t%2000==0)s.sample(t,o);
    auto d=s.update(t,true,100,o);assert(d.effective<=100&&d.effective>=0);
    assert(d.effective-last<=Policy::RisePctPerMin*5*.001/60+.00002);
    assert(last-d.effective<=Policy::FallPctPerMin*5*.001/60+.00002);last=d.effective;
  }
  o.vent=true;auto windows=s.observer().estimates().windows;
  for(uint32_t t=1800000;t<1900000;t+=2000){s.sample(t,o);s.update(t,true,100,o);}
  assert(s.observer().estimates().windows==windows);
  for(unsigned fault=0;fault<8;++fault){
    auto bad=input();switch(fault){case 0:bad.sensor=false;break;case 1:bad.safety=true;break;
      case 2:bad.tune=true;break;case 3:bad.test=true;break;case 4:bad.maintenance=true;break;
      case 5:bad.recovery=true;break;case 6:bad.pv=NAN;break;case 7:bad.raw=INFINITY;break;}
    s.sample(2000000+fault*2000,bad);auto d=s.update(2000000+fault*2000,true,50,bad);
    assert(d.state==State::FaultBypass&&!d.cooling&&d.effective==50);
    assert(s.observer().estimates().windows==windows);
  }
  MachineConfig cfg;ThermalController pid;
  for(uint32_t t=1000;t<300000;t+=2000){cfg.maxHeaterPower=t<100000?70:t<200000?40:60;
    const float power=pid.updateOnNewSample(t,37.5,34,cfg,true);assert(power<=cfg.maxHeaterPower&&std::isfinite(power));}
  assert(pid.updateOnNewSample(302000,37.5,39,cfg,true)==0);
}
static void selfHeating(){
  for(float resolution:{.1f,.01f})for(bool heat:{false,true}){
    ActualSensorFilter filter;
    AdaptiveThermalSupervisor s;auto o=input();bool detected=false;uint32_t lastTransition=0;bool lastCool=false;
    for(uint32_t t=0;t<900000;t+=5){
      s.tick(t,false);
      if(t%2000==0){
        o.raw=std::round((heat?37.5f+t*.0000005f:37.5f+std::sin(t*.0001f)*.015f)/resolution)*resolution;
        filter.updateFilter(o.raw,60);o.pv=filter.value();s.sample(t,o);
      }
      auto d=s.update(t,true,100,o);
      if(d.cooling!=lastCool){assert(since(t,lastTransition)>=Policy::CoolingMinMs);lastTransition=t;lastCool=d.cooling;}
      if(d.selfHeating){detected=true;assert(d.effective==0);assert(s.effectiveConfidence()>=Policy::HighConfidence);}
    }
    assert(detected==heat);if(heat)assert(s.decision().cooling);
    o.pv=o.raw=37.5;auto d=s.update(1000000,true,100,o);assert(!d.selfHeating&&!d.cooling);
  }
}
static void persistence(){
  Estimates e;e.confidence=90;e.load=.5;e.coast=.2;e.coastSec=30;e.hold=20;e.windows=8;
  Model m=makeModel(e,123,1800000000,1);assert(validModel(m));
  assert(compatibleModel(m,123,1800000010,false));
  assert(!compatibleModel(m,124,1800000010,false));assert(!compatibleModel(m,123,1800000010,true));
  assert(!compatibleModel(m,123,1800000000+Policy::ModelMaxAgeSec+1,false));
  for(size_t b=0;b<sizeof(Model);++b){Model bad=m;reinterpret_cast<uint8_t *>(&bad)[b]^=1;assert(!validModel(bad));}
  Preferences::records().clear();ModelStorage store;store.service(0);store.offer(m);
  store.service(Policy::SaveMinMs-1);assert(Preferences::records()["a"].empty());store.service(Policy::SaveMinMs);
  ModelStorage reboot;reboot.service(0);Model seed;assert(reboot.takeSeed(seed)&&validModel(seed));
  for(int cut=0;cut<static_cast<int>(sizeof(Model));++cut){
    Preferences::budget()=cut;store.offer(m);store.service((cut+2)*Policy::SaveMinMs);
    ModelStorage interrupted;interrupted.service(0);assert(interrupted.takeSeed(seed)&&validModel(seed));
    if(cut>0)assert(interrupted.takeInvalid()&&!interrupted.takeInvalid());
  }
  Preferences::budget()=-1;
}
static void validityAndCoast(){
  AdaptiveThermalSupervisor immediate;qualify(immediate);auto invalid=input();invalid.sensor=false;
  immediate.update(1500001,true,100,invalid); // no new sensor sample
  assert(immediate.observer().estimates().confidence==0&&!immediate.observer().estimates().learningValid);

  for(unsigned gate=0;gate<11;++gate){
    ThermalObserver observer;auto o=input();
    switch(gate){case 0:o.sensor=false;break;case 1:o.fanStable=false;break;case 2:o.vent=true;break;
      case 3:o.cooling=true;break;case 4:o.tune=true;break;case 5:o.safety=true;break;
      case 6:o.recovery=true;break;case 7:o.test=true;break;case 8:o.maintenance=true;break;
      case 9:o.raw=o.high-.1f;break;case 10:o.pv=NAN;break;}
    for(uint32_t t=0;t<900000;t+=2000){observer.tick(t,t%4000==0);observer.sample(t,o);}
    assert(observer.estimates().windows==0&&!observer.estimates().learningValid);
  }
  ThermalObserver observer;auto o=input();o.sp=37.5;
  for(uint32_t t=0;t<1500000;t+=5){
    const uint32_t phase=t%480000;const bool on=phase>=180000&&phase<240000;
    observer.tick(t,on);
    if(t%2000==0){
      o.pv=o.raw=phase<180000?30:phase<240000?30+(phase-180000)*.00002f:
        phase<300000?31.2f+(phase-240000)*.000005f:31.5f-(phase-300000)*.000001f;
      observer.sample(t,o);
    }
  }
  assert(observer.estimates().coastWindows>=2);
  assert(observer.estimates().coast>.2f&&observer.estimates().coast<.4f);
  AdaptiveThermalSupervisor wrapped;qualify(wrapped,UINT32_MAX-1000000);assert(wrapped.decision().effective<100);
  auto valid=input();const auto prior=wrapped.observer().estimates().windows;
  wrapped.invalidate();assert(wrapped.observer().estimates().windows==prior);
  assert(wrapped.observer().estimates().validMs==0);
  wrapped.sample(500000,valid);auto d=wrapped.update(500000,true,100,valid,true);
  assert(d.effective==100&&!d.cooling); // explicit read-only commissioning build
}
static void actualSafety(){
  for(unsigned reason=0;reason<9;++reason){
    TuneHarness h;h.batchRunning_=true;h.config_.adaptiveThermalBalanceEnabled=true;
    for(uint32_t t=1000;t<30000;t+=5)h.cycle(t,t%2000==0);
    switch(reason){case 0:h.highTemperatureActive_=h.faults_.cooling=h.faults_.inhibit=true;break;
      case 1:h.emergencyActive_=h.faults_.cooling=true;break;case 2:h.sensorUsable_=false;break;
      case 3:h.faults_.drop=true;break;case 4:h.faults_.inhibit=true;break;case 5:trip=true;break;
      case 6:maintenance=true;break;case 7:h.storageFaultLatched_=true;break;
      case 8:h.inputs_.in.heaterEnable=false;break;}
    h.cycle(30000);assert(!h.outputs_.state().heaterSsr);
    if(reason<2)assert(h.outputs_.state().ventFan); // adaptive never cancels safety fan
    trip=maintenance=false;
    h.config_.adaptiveThermalBalanceEnabled=false;h.cycle(30005,false);
    assert(!h.adaptiveCoolingRequested());if(reason<2)assert(h.outputs_.state().ventFan);
  }
}
static void cpuBudget(){
  TuneHarness h;h.batchRunning_=true;h.config_.adaptiveThermalBalanceEnabled=true;
  uint64_t maxNs=0,totalNs=0;
  for(uint32_t t=1000;t<101000;t+=5){
    timespec before{},after{};clock_gettime(CLOCK_THREAD_CPUTIME_ID,&before);
    h.cycle(t,t%2000==0);clock_gettime(CLOCK_THREAD_CPUTIME_ID,&after);
    const uint64_t ns=static_cast<uint64_t>(after.tv_sec-before.tv_sec)*1000000000ULL+after.tv_nsec-before.tv_nsec;
    maxNs=std::max(maxNs,ns);totalNs+=ns;
  }
  assert(maxNs/1000<CONTROL_CYCLE_TRIP_US);
  assert(totalNs/20000/1000<CONTROL_TASK_PERIOD_MS*1000);
  std::printf("Host actual adaptive route CPU max_us=%llu mean_us=%.3f; ESP32 task latency remains physical qualification\n",
    static_cast<unsigned long long>(maxNs/1000),totalNs/20000/1000.0);
}
int main(){rollback();envelopes();selfHeating();persistence();validityAndCoast();actualSafety();cpuBudget();
  std::puts("Adaptive actual heating OFF waveform oracle, limits/AW/slew, invalid gates, self-heating/noise, cooling hysteresis, CRC/age/signature/all-byte power cuts PASS");}
