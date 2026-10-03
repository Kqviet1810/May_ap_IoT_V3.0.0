#include "thermal-fixture.h"
#include "actual-filter.inc"
#include "fixtures/thermal-v1/sensor_filter.h"
int main(){
  OldSensorFilter old;ActualSensorFilter current;
  const int16_t sequence[]={375,375,376,375,374,375};
  double maximum=0,sum=0,squares=0;
  constexpr unsigned count=60000;
  for(unsigned n=0;n<count;++n){
    // Fixed noise, ramps, steps and long flat tails using genuine X10 registers.
    int16_t raw=sequence[n%6];
    if(n>=10000 && n<20000) raw=static_cast<int16_t>(300+(n-10000)/200);
    if(n>=20000 && n<30000) raw=static_cast<int16_t>(400-(n-20000)/200);
    if(n>=30000) raw=n<45000?320:375;
    old.updateFilter(raw,600);current.updateFilter(raw/10.0f,60);
    const double delta=std::fabs(old.value()-current.value());
    maximum=std::max(maximum,delta);sum+=delta;squares+=delta*delta;
    assert(delta<=0.002); // Q8 truncation bound, not bit equality or sensor accuracy.
  }
  std::printf("FILTER,%u,%.9f,%.9f,%.9f\n",count,maximum,sum/count,std::sqrt(squares/count));
  std::puts("Historical Q8 vs actual float: X10 filtered estimate delta <=0.002 C; no native precision claim PASS");
}
