#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_observer.h"
#include <cassert>
#include <cstdio>
using namespace MayapAdaptive;
int main(){
  Observation o;o.sensor=o.fanStable=true;o.pv=o.raw=o.sp=37.5f;o.high=38.2f;
  ThermalObserver observer;
  for(uint32_t ms=0;ms<1500000;ms+=5){observer.tick(ms,ms%1000<300);if(ms%2000==0)observer.sample(ms,o);}
  auto e=observer.estimates();assert(e.holdWindows>=5&&e.confidence>=50);assert(std::fabs(e.hold-30)<.01);
  const auto windows=e.windows;o.vent=true;for(uint32_t ms=1500000;ms<1800000;ms+=2000)observer.sample(ms,o);
  assert(observer.estimates().windows==windows&&!observer.estimates().learningValid);
  o.vent=false;o.sp=36.0f;observer.sample(1800000,o);
  assert(observer.estimates().windows==windows&&!observer.estimates().learningValid);
  o.sp=37.5f;o.raw=36.8f;o.pv=37.45f;observer.sample(1802000,o);
  assert(observer.estimates().windows==windows&&!observer.estimates().learningValid);
  o.sensor=false;observer.sample(1804000,o);assert(observer.estimates().confidence==0);
  assert(!ThermalObserver::finiteSeed(NAN,0,0,0));
  observer.reset();observer.seed(.4,.2,30,20);assert(observer.estimates().confidence==10&&observer.estimates().windows==0);
  ThermalObserver wrap;wrap.tick(UINT32_MAX-4,true);wrap.tick(5,true);assert(wrap.actualOnMs()==10);
  std::puts("Adaptive observer actual energy, hold windows, invalid gates, low-trust seed and wrap PASS");
}
