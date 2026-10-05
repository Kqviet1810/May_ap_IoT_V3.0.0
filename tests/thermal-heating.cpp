#include "thermal-heating-harness.h"
static void assertOff(const Harness &h){assert(!h.outputs_.state().heaterSsr);}
static std::vector<bool> legacyCycleWaveform(uint16_t cycle) {
  Harness h;h.config_.pidCycleSec=cycle;h.config_.maxHeaterPower=30;h.outputs_.begin();
  std::vector<bool> result;
  for(uint32_t now=1000;now<121000;now+=5){h.cycle(now,now%2000==0);result.push_back(h.outputs_.state().heaterSsr);}
  return result;
}
int main(){
  assert(legacyCycleWaveform(1)==legacyCycleWaveform(10));
  assert(legacyCycleWaveform(60)==legacyCycleWaveform(10));
  Harness pickup;pickup.outputs_.begin();pickup.cycle(1000);
  assertOff(pickup);assert(pickup.runtime_.heaterPower==0 && pickup.pid_.output()==0);
  for(unsigned reason=0;reason<16;++reason){
    Harness h;h.warm();assert(h.outputs_.state().heaterSsr && h.runtime_.heaterPower==100);
    switch(reason){
      case 0:h.inputs_.in.heaterEnable=false;break;
      case 1:h.sensorUsable_=false;break;
      case 2:h.faults_.drop=true;break;
      case 3:h.faults_.inhibit=true;break;
      case 4:h.emergencyActive_=true;break;
      case 5:h.ventTemperatureActive_=true;break;
      case 6:h.storageFaultLatched_=true;break;
      case 7:h.safetyJournalFaultLatched_=true;break;
      case 8:h.batchClearPending_=true;break;
      case 9:h.abnormalResetLatched_=true;break;
      case 10:h.batchRunning_=false;break;
      case 11:maintenance=true;break;
      case 12:bootReady=false;break;
      case 13:trip=true;break;
      case 14:h.fanOnSince_=15001;break;
      case 15:h.resumeConfirmationRequired_=true;break;
    }
    h.cycle(15001);assertOff(h);assert(h.pid_.output()==0);
    bootReady=true;trip=maintenance=false;
    h.inputs_.in.heaterEnable=true;h.sensorUsable_=true;h.faults_=FakeFaults{};
    h.emergencyActive_=h.ventTemperatureActive_=h.storageFaultLatched_=h.safetyJournalFaultLatched_=false;
    h.batchClearPending_=h.abnormalResetLatched_=h.resumeConfirmationRequired_=false;
    h.batchRunning_=true;h.fanOnSince_=1000;
    // Resume with a tiny genuine demand: no old 100%-burst or credit can return.
    h.temperature_=h.config_.targetTemp-0.01f;
    h.cycle(25000);h.cycle(25000+HEAT_MASTER_PICKUP_MS+1);
    assertOff(h);
  }
  Harness h;h.temperature_=29;h.warm();h.config_.targetTemp=30;
  h.cycle(16000);assert(h.runtime_.heaterPower>=0 && h.runtime_.heaterPower<100);
  assert(std::fabs(h.runtime_.heaterPower-h.pid_.output())<0.001f); // requested %, not instantaneous SSR
  // Autotune actual controller route: 30% total passes through the scheduler.
  Harness tune;tune.batchRunning_=false;tune.autotune_.configure(37.5f);tune.autotune_.start(1000,37.4f);
  MachineConfig result;tune.temperature_=37.4f;
  tune.autotune_.update(1000,37.4f,tune.config_,result);tune.warm();
  unsigned energy=0;
  for(uint32_t t=16000;t<616000;t+=50){
    tune.cycle(t,false);
    energy+=tune.outputs_.state().heaterSsr;
    assert(tune.runtime_.heaterPower==30);
  }
  const double delivered=100.0*energy/12000;
  assert(std::fabs(delivered-30)<0.2);
  tune.faults_.inhibit=true;tune.cycle(616001,false);assertOff(tune);
  for(unsigned reason=0;reason<15;++reason){
    Harness guarded;guarded.batchRunning_=false;guarded.temperature_=37.4f;
    guarded.autotune_.configure(37.5f);guarded.autotune_.start(1000,37.4f);
    guarded.autotune_.update(1000,37.4f,guarded.config_,result);guarded.warm();
    // Advance until a 30% burst is physically ON, then cut inside that burst.
    uint32_t at=15000;
    while(!guarded.outputs_.state().heaterSsr && at<25000)guarded.cycle(at+=50,false);
    assert(guarded.outputs_.state().heaterSsr);
    switch(reason){
      case 0:guarded.inputs_.in.heaterEnable=false;break;
      case 1:guarded.inputs_.in.autoMode=false;break;
      case 2:guarded.sensorUsable_=false;break;
      case 3:guarded.faults_.inhibit=true;break;
      case 4:guarded.faults_.drop=true;break;
      case 5:guarded.highTemperatureActive_=true;break;
      case 6:guarded.emergencyActive_=true;break;
      case 7:guarded.storageFaultLatched_=true;break;
      case 8:guarded.storageDegraded_=true;break;
      case 9:guarded.abnormalResetLatched_=true;break;
      case 10:guarded.safetyJournalFaultLatched_=true;guarded.faults_.drop=true;break;
      case 11:guarded.batchClearPending_=true;guarded.faults_.drop=true;break;
      case 12:guarded.fanOnSince_=at+1;break;
      case 13:maintenance=true;break;
      case 14:trip=true;break;
    }
    guarded.cycle(at+1,false);assertOff(guarded);
    trip=maintenance=false;
  }
  h.testModeActive_=true;h.cycle(17000);assert(h.testCalls==1);assertOff(h);
  std::puts("Actual heating route: 16 PID + 15 autotune safety/permit cuts, no windup/backlog, pickup/fan/storage/OTA/boot/trip, total requested %, autotune PDM PASS");
}
