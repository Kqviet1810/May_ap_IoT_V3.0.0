// Execute the actual MachineController heating method, not a mirrored gate expression.
#include "thermal-fixture.h"
#include <deque>
#include <cstddef>
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_control.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/heater_burst_scheduler.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/startup_output_policy.h"
static uint32_t clockMs=0;
uint32_t millis() { return clockMs; }
bool timeReached(uint32_t now,uint32_t then) { return static_cast<int32_t>(now-then)>=0; }
constexpr int LOW=0,HIGH=1,OUTPUT=2;
static int levels[64]{};
static bool bootReady=true,trip=false,maintenance=false;
bool mayapBootOperationsReady() { return bootReady; }
bool mayapSystemTripLatched() { return trip; }
bool mayapFirmwareMaintenanceActive() { return maintenance; }
void digitalWrite(uint8_t pin,int value) { assert(pin<64);levels[pin]=value; }
void pinMode(uint8_t pin,int mode) { assert(pin<64 && mode==OUTPUT && levels[pin]==LOW); }
template<class T,unsigned N> class FixedRing {
 public:
  void push(const T &v) { if(q.size()==N){q.pop_front();++overflow;}q.push_back(v); }
  bool pop(T &v){if(q.empty())return false;v=q.front();q.pop_front();return true;}
  uint32_t overflowCount()const{return overflow;}
 private:std::deque<T> q;uint32_t overflow=0;
};
#include "actual-output.inc"
#include "actual-heating-constants.inc"
#include "actual-heating-config.inc"
#ifdef MAYAP_HEATER_SSR_B_PIN
constexpr uint8_t HEATER_GROUP_COUNT=2;
#else
constexpr uint8_t HEATER_GROUP_COUNT=1;
#endif
struct InputState { bool light=false,autoMode=true,circulationFan=true,heaterEnable=true; };
struct FakeInputs { InputState in; const InputState &state()const{return in;} };
struct FakeRtc { bool valid()const{return false;} uint32_t epoch()const{return 0;} };
struct FakeFaults {
  bool drop=false,inhibit=false,cooling=false;
  bool masterDropRequired()const{return drop;}
  bool ssrInhibited()const{return inhibit;}
  bool circulationForced()const{return cooling;}
  bool ventForced()const{return cooling;}
};
uint8_t ventProfileDutyPercent(const HeatingConfig &,uint32_t,bool){return 0;}
enum class TurnPhase { Idle,MovingLeft,MovingRight };
struct Harness {
  FakeInputs inputs_;FakeRtc rtc_;FakeFaults faults_;
  HeatingConfig config_;
  OutputArbiter outputs_;
  ThermalController pid_; RelayAutoTune autotune_;
  HeaterBurstScheduler heaterBurst_{HEATER_GROUP_COUNT};
  struct {float heaterPower=0;} runtime_;
  bool testModeActive_=false,lightWebOverrideActive_=false,lightWebOverrideRefInput_=false,lightWebOverrideValue_=false;
  bool resumeConfirmationRequired_=false,batchRunning_=true,prevBatchRunningForOutputs_=false;
  bool sensorUsable_=true,humidityLowActive_=false,previousFanCommand_=false;
  bool emergencyActive_=false,highTemperatureActive_=false,ventTemperatureActive_=false;
  bool batchClearPending_=false,safetyJournalFaultLatched_=false,storageFaultLatched_=false,storageDegraded_=false,abnormalResetLatched_=false;
  bool newSensorSample_=true,batchOverdueSirenActive_=false,sirenSelfTestActive_=false;
  uint32_t circFanStaggerUntil_=0,postCoolUntil_=0,sensorStartupGraceUntil_=0,fanOnSince_=0,heatRestartNotBefore_=0,sirenMutedUntil_=0;
  float temperature_=25,humidity_=60,pidPower_=0;
  TurnPhase turnPhase_=TurnPhase::Idle;
  unsigned testCalls=0;
  uint32_t elapsedBatchSec(uint32_t)const{return 0;}
  void updateTestModeOutputs(uint32_t now){++testCalls;outputs_.forceSafe(now);}
#include "actual-heating.inc"
  void cycle(uint32_t now,bool newSample=true){clockMs=now;newSensorSample_=newSample;updateHeatingAndOutputs(now);}
  void warm(){outputs_.begin();for(uint32_t t=1000;t<=15000;t+=50)cycle(t,t%2000==0);}
};
static void assertOff(const Harness &h){assert(!h.outputs_.state().heaterSsrA && !h.outputs_.state().heaterSsrB && !h.outputs_.state().heaterSsr);}
int main(){
  Harness pickup;pickup.outputs_.begin();pickup.cycle(1000);
  assertOff(pickup);assert(pickup.runtime_.heaterPower==0 && pickup.pid_.output()==0);
  for(unsigned reason=0;reason<16;++reason){
    Harness h;h.warm();assert(h.outputs_.state().heaterSsrA && h.runtime_.heaterPower==100);
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
  h.cycle(16000);assert(h.runtime_.heaterPower>0 && h.runtime_.heaterPower<100);
  assert(std::fabs(h.runtime_.heaterPower-h.pid_.output())<0.001f); // requested %, not instantaneous SSR
  // Autotune actual controller route: 30% total passes through the scheduler.
  Harness tune;tune.batchRunning_=false;tune.autotune_.configure(37.5f);tune.autotune_.start(1000,35);
  MachineConfig result;tune.temperature_=35;
  tune.autotune_.update(1000,35,tune.config_,result);tune.warm();
  unsigned energy=0;
  for(uint32_t t=16000;t<616000;t+=50){
    tune.cycle(t,false);
    energy+=tune.outputs_.state().heaterSsrA+tune.outputs_.state().heaterSsrB;
    assert(tune.runtime_.heaterPower==30);
  }
  const double delivered=100.0*energy/(12000*HEATER_GROUP_COUNT);
  assert(std::fabs(delivered-30)<0.2);
  tune.faults_.inhibit=true;tune.cycle(616001,false);assertOff(tune);
  for(unsigned reason=0;reason<15;++reason){
    Harness guarded;guarded.batchRunning_=false;guarded.temperature_=35;
    guarded.autotune_.configure(37.5f);guarded.autotune_.start(1000,35);
    guarded.autotune_.update(1000,35,guarded.config_,result);guarded.warm();
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
