// Execute the actual E115 full-bank energy accounting body from MachineController.
#include "thermal-fixture.h"
struct E115Harness {
  struct { float targetTemp=37.5f,tempHysteresis=0.2f,heaterStuckMinRiseC=0.2f;
           uint16_t heaterStuckDurationSec=60; } config_;
  struct Input { bool heaterEnable=true; };
  struct Inputs { Input input;
           const Input &state() const { return input; } } inputs_;
  struct Output { bool heaterSsr=false; };
  struct Outputs { Output output;
           const Output &state() const { return output; } } outputs_;
  bool batchRunning_=true,sensorUsable_=true,heaterStuckTracking_=false;
  bool heaterNotHeatingActive_=false;
  float temperature_=30,heaterStuckStartTemp_=NAN;
  uint32_t heaterStuckSinceAt_=0,heaterStuckAccumOnMs_=0;
  void update(uint32_t now) {
#include "actual-heater-evidence.inc"
  }
};
int main() {
  E115Harness bank;
  bank.update(0);
  bank.outputs_.output.heaterSsr=true;
  for(uint32_t t=1000;t<60000;t+=1000) {
    bank.update(t);
    assert(!bank.heaterNotHeatingActive_);
  }
  bank.update(60000);
  assert(bank.heaterStuckAccumOnMs_==60000 && bank.heaterNotHeatingActive_);
  bank.sensorUsable_=false;bank.update(60001);
  assert(!bank.heaterNotHeatingActive_ && bank.heaterStuckAccumOnMs_==0);
  E115Harness pulsed;
  pulsed.update(0);
  for(uint32_t t=1000;t<=120000;t+=1000) {
    pulsed.outputs_.output.heaterSsr=((t/1000)%2)==0;
    pulsed.update(t);
  }
  assert(pulsed.heaterStuckAccumOnMs_==60000 && pulsed.heaterNotHeatingActive_);
  std::puts("Actual E115: GPIO1 ON counts as full 16 kW bank; 60 s ON triggers, OFF/cut reset PASS");
}
