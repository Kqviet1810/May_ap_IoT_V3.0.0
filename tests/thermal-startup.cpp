#include "thermal-fixture.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_control.h"
int main() {
  ThermalStartupController startup;
  startup.observe(1000,false);
  auto first=startup.decide(1000,37.5f,25.0f,100.0f);
  assert(first.phase==ThermalStartupController::Phase::FullHeat);
  assert(first.ceiling>0 && first.ceiling<=100);
  // The sensor stays flat during a delayed heat pulse. Meter the actual SSR
  // ON time; braking must happen before filtered PV crosses the setpoint.
  startup.observe(1100,true);
  bool braked=false;
  for(uint32_t t=1200;t<=160000;t+=100){
    startup.observe(t,true);
    if(t%2000==0){
      const auto d=startup.decide(t,37.5f,25.0f,100.0f);
      assert(std::isfinite(d.ceiling)&&d.ceiling>=0&&d.ceiling<=100);
      if(d.ceiling<1.0f){braked=true;break;}
    }
  }
  assert(braked && startup.predictedPeak()>25.0f);
  startup.reset();
  startup.observe(1000,false);
  startup.decide(1000,37.5f,37.3f,100);
  // A momentarily stable PV enters HOLD, but HOLD must retain braking.
  for(uint32_t t=3000;t<=70000;t+=2000){
    startup.observe(t,false); // a missed long poll resets energy history safely
    const auto d=startup.decide(t,37.5f,37.3f,100);
    assert(d.ceiling>=0 && d.ceiling<=100);
  }
  assert(startup.phase()==ThermalStartupController::Phase::Hold);
  startup.observe(70100,true);
  for(uint32_t t=70200;t<=100000;t+=100)startup.observe(t,true);
  auto hold=startup.decide(100000,37.5f,37.3f,100);
  assert(hold.phase==ThermalStartupController::Phase::Hold);
  assert(hold.ceiling<1.0f); // actual heat history can stop HOLD output.
  startup.observe(105000,false);
  assert(startup.sampleStale(105000));
  startup.decide(105000,37.5f,37.3f,100);
  assert(!startup.sampleStale(105000));
  startup.reset();
  startup.observe(UINT32_MAX-100U,false);
  startup.observe(50U,true);
  const auto wrapped=startup.decide(50U,37.5f,30.0f,100);
  assert(std::isfinite(wrapped.ceiling)&&wrapped.ceiling>=0&&wrapped.ceiling<=100);
  std::puts("Thermal startup: phases, pre-PV braking, HOLD brake and millis rollover PASS");
}
