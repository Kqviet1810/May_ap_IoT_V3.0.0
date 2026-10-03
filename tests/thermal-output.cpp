#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <cstddef>
#include "../MAYAP_INDUSTRIAL_v1_0_0/startup_output_policy.h"
static uint32_t clockMs=0;
uint32_t millis() { return clockMs; }
uint32_t elapsedMs(uint32_t now,uint32_t then) { return now-then; }
bool timeReached(uint32_t now,uint32_t then) { return static_cast<int32_t>(now-then)>=0; }
constexpr int LOW=0,HIGH=1,OUTPUT=2;
static int levels[64]{};
static bool bootReady=true, trip=false;
bool mayapBootOperationsReady() { return bootReady; }
bool mayapSystemTripLatched() { return trip; }
void digitalWrite(uint8_t pin,int value) { assert(pin<64); levels[pin]=value; }
void pinMode(uint8_t pin,int mode) { assert(pin<64 && mode==OUTPUT && levels[pin]==LOW); }
template<class T, unsigned N> class FixedRing {
 public:
  void push(const T &v) { if(q.size()==N) { q.pop_front(); ++overflow; } q.push_back(v); }
  bool pop(T &v) { if(q.empty())return false; v=q.front();q.pop_front();return true; }
  uint32_t overflowCount() const { return overflow; }
 private: std::deque<T> q; uint32_t overflow=0;
};
#include "actual-output.inc"
static void assertOff(const OutputArbiter &arbiter) {
  assert(!arbiter.state().heaterSsr);
  assert(levels[PIN_OUT_HEATER_SSR]==LOW);
}
int main() {
  OutputArbiter arbiter;
  arbiter.begin(); assertOff(arbiter);
  OutputRequest request;
  request.heaterSsr=true;
  request.heatMaster=true;request.circulationFan=true;
  bootReady=false;arbiter.update(1000,request);assertOff(arbiter);
  assert(!arbiter.state().heatMaster);
  bootReady=true;arbiter.update(1001,request);assertOff(arbiter);
  assert(!arbiter.heaterReady(1001));
  arbiter.update(1001+HEAT_MASTER_PICKUP_MS,request);
  assert(arbiter.state().heaterSsr && levels[PIN_OUT_HEATER_SSR]==HIGH);
  // SSR inhibit has immediate priority even while contactor remains permitted.
  request.heaterSsr=false;arbiter.update(1002+HEAT_MASTER_PICKUP_MS,request);assertOff(arbiter);
  request.heaterSsr=true;arbiter.update(1003+HEAT_MASTER_PICKUP_MS,request);
  trip=true;arbiter.update(1004+HEAT_MASTER_PICKUP_MS,request);assertOff(arbiter);
  assert(!arbiter.state().heatMaster && !arbiter.state().circulationFan);
  trip=false;
  OutputArbiter mechanical;
  mechanical.begin(); request=OutputRequest{};
  request.heatMaster=true;
  for(uint32_t t=1000;t<3500000;t+=1000) {
    request.heaterSsr=((t/1000)%2)==0;
    mechanical.update(t,request);
    OutputEvent e;while(mechanical.popEvent(e)){}
  }
  assert(!mechanical.relayRateExceeded() && mechanical.transitionsThisHour()==1);
  OutputArbiter chatter;
  chatter.begin();request=OutputRequest{};
  for(uint32_t t=1000;t<2002000;t+=2000) {
    request.light=request.humidifier=((t/2000)%2)==0;
    chatter.update(t,request);
    OutputEvent e;while(chatter.popEvent(e)){}
  }
  assert(chatter.relayRateExceeded()); // Mechanical wear protection is still active.
  request.heatMaster=request.heaterSsr=true;
  chatter.update(2003000,request);
  chatter.update(2003000+HEAT_MASTER_PICKUP_MS,request);
  chatter.forceSafe(2003001+HEAT_MASTER_PICKUP_MS);assertOff(chatter);
  assert(!chatter.state().light && !chatter.state().humidifier);
  // Contactor pickup and shutdown across millis rollover.
  clockMs=UINT32_MAX-1000;
  OutputArbiter wrap;wrap.begin();request=OutputRequest{};
  request.heatMaster=request.heaterSsr=true;
  wrap.update(clockMs,request);
  wrap.update(clockMs+HEAT_MASTER_PICKUP_MS,request);assert(wrap.state().heaterSsr);
  request.heatMaster=false;request.immediateMasterDrop=true;
  wrap.update(clockMs+HEAT_MASTER_PICKUP_MS+1,request);assertOff(wrap);
  assert(!wrap.state().heatMaster);
  std::puts("Actual OutputArbiter: one 16 kW GPIO1 bank, boot/trip/inhibit/immediate OFF/pickup/relay wear/millis wrap PASS");
}
