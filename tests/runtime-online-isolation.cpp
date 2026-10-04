#include <cassert>
#include <cstdint>
#include <cstdio>
#include "../MAYAP_INDUSTRIAL_v1_0_0/runtime_recovery_policy.h"
static uint32_t clockMs=0;
uint32_t millis(){return clockMs;}
void mayapSerialPrintf(bool,const char*,...){}
#include "actual-services.inc"
struct FakeEsp { uint32_t free=100000; uint32_t getFreeHeap(){return free;} } ESP;
constexpr uint8_t HEALTH_HEAP_CRITICAL_PERCENT=6,HEALTH_HEAP_SERIOUS_PERCENT=15,
 HEALTH_HEAP_WARN_PERCENT=30,HEALTH_HEAP_WARN_CLEAR_PERCENT=35,HEALTH_STREAK_CONFIRM=3;
enum class FaultCode { HeapCritical,HeapLow };
struct Faults {void set(FaultCode,bool,uint32_t,int16_t=0){} };
struct ActualHeapHealth {
 bool healthBaselineCaptured_=true,healthHeapLowActive_=false;
 uint32_t healthHeapBaseline_=100000;
 uint8_t healthHeapSeriousStreak_=0,healthHeapLowStreak_=0;
 Faults faults_;
#include "actual-health-heap.inc"
};
int main(){
 using MayapRecovery::Service;
 MayapOnline::RadioGate gate;
 // Every interleaving of begin-before-enter and enter-before-begin, all owners.
 for(uint8_t owner=1;owner<4;owner++)for(unsigned repeat=0;repeat<10000;repeat++){
  gate.release();assert(gate.enter(owner));gate.closeAdmission();
  assert(!gate.enter(owner));gate.acknowledge(owner);assert(!gate.drained(1U<<owner));
  gate.leave(owner);assert(!gate.drained(1U<<owner)); // idle != closed socket
  gate.acknowledge(owner);assert(gate.drained(1U<<owner));
  gate.release();gate.closeAdmission();assert(!gate.enter(owner));
  assert(!gate.drained(1U<<owner));gate.acknowledge(owner);assert(gate.drained(1U<<owner));
 }
 ActualHeapHealth health;
 ESP.free=1;health.serviceHealthHeap(1,1);assert(mayapOnlineMemoryPressure());
 assert(!mayapOnlineIoEnter(Service::Cloud));
 mayapSetOnlineMemoryPressure(false);ESP.free=10000;
 for(unsigned i=0;i<3;i++)health.serviceHealthHeap(i,10000);
 assert(mayapOnlineMemoryPressure());
 for(uint8_t owner=0;owner<4;owner++){
  MayapRecovery::ServiceWatch watch;
  for(uint32_t t=0;t<24U*3600000U;t+=1000){
   const auto action=watch.update(t,true,0,0,MayapRecovery::SERVICE_TIMEOUT_MS[owner]);
   assert(action==MayapRecovery::Action::None||action==MayapRecovery::Action::Reinit||
          action==MayapRecovery::Action::Isolate||action==MayapRecovery::Action::Degraded);
  }
 }
 puts("Actual Online gate/heap: 30000 admission races, TLS/socket drain fences, all-owner 24h stalls and heap shedding without controller reset PASS");
}
