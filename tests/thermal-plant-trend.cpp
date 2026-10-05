#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_plant_identifier.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <vector>
using std::isfinite;
#include "../MAYAP_INDUSTRIAL_v1_0_0/heater_burst_scheduler.h"

struct Plant { const char *name; double capacity,loss; };
struct SensorFilter {
  double window[3]{},filtered=0;unsigned count=0,index=0;bool ready=false;
  double update(double value){window[index]=value;index=(index+1)%3;if(count<3)++count;
    double sorted[3]{window[0],window[1],window[2]};
    const unsigned n=count>3?3:count;std::sort(sorted,sorted+n);
    const double median=sorted[n/2];if(!ready){filtered=median;ready=true;}else filtered+=(median-filtered)*3.0/8.0;return filtered;}
};
struct CaseResult { ThermalPlantModel model; double baselineSlope, duTrue, kTrue; };

static CaseResult runCase(const Plant &plant,unsigned dead,double resolution,double baselineSlope,bool positive,bool noisy=false,double stepMag=0.10){
  constexpr uint32_t rawDtMs=100U,bucketMs=10000U,stepAtMs=900000U,endMs=1800000U;
  constexpr double dt=0.1,heaterWatts=16000.0,baseTemp=37.0;
  const double u0=positive?0.10:0.20; const double u1=positive?u0+stepMag:u0-stepMag;
  const double du=u1-u0; const double kTrue=heaterWatts/plant.capacity;
  const size_t delayN=std::max<size_t>(1,static_cast<size_t>(std::lround(dead/dt)));
  std::vector<double> delay(delayN,0.0);size_t cursor=0;
  HeaterBurstScheduler pdm(1U,300U);ThermalDeliveredEnergyBucket bucket;ThermalPlantIdentifier id;SensorFilter filter;
  bool ssr=false;bucket.reset(0U,false);double response=0.0;uint32_t noiseState=0x12345678U;
  for(uint32_t now=rawDtMs;now<=endMs;now+=rawDtMs){
    const double request=(now>stepAtMs?u1:u0)*100.0;
    ssr=pdm.update(now,static_cast<float>(request),true).groupA;
    assert(bucket.observe(now,ssr));
    const double differential=(ssr?1.0:0.0)-u0;
    const double delayed=delay[cursor];delay[cursor]=differential;cursor=(cursor+1U)%delay.size();
    response+=(heaterWatts*delayed-plant.loss*response)/plant.capacity*dt;
    const double t=now*0.001;
    double pv=baseTemp+baselineSlope*t+response;
    if(noisy){noiseState=noiseState*1664525U+1013904223U;pv+=((noiseState>>8)&0xffff)/65535.0*0.7-0.35;}
    const double sampled=std::round(pv/resolution)*resolution;
    const double filtered=filter.update(sampled);
    if(now%bucketMs==0U){float actual=NAN;assert(bucket.close(now,actual));assert(id.addSample(now,static_cast<float>(filtered),actual));}
  }
  return {id.identify(),baselineSlope,du,kTrue};
}

int main(){
  const Plant plants[]={{"light",180000,120},{"medium",600000,180},{"heavy",1600000,300}};
  unsigned total=0,fail=0;
  std::map<std::tuple<std::string,unsigned,int,int>,std::vector<std::pair<double,ThermalPlantModel>>> equivalence;
  std::puts("plant,dead_s,resolution,true_baseline_slope,estimated_baseline_slope,step_sign,true_kPrime,estimated_kPrime,true_theta,estimated_theta,confidence,result");
  for(const auto &plant:plants)for(unsigned dead:{5U,15U,30U,60U})for(double resolution:{0.1,0.01})
    for(double slope:{-0.0015,0.0,0.0015})for(bool positive:{true,false}){
      const auto r=runCase(plant,dead,resolution,slope,positive);++total;
      const bool slopePass=isfinite(r.model.baselineSlope)&&std::fabs(r.model.baselineSlope-slope)<=0.00025;
      const bool pass=r.model.valid&&r.model.mode==ThermalPlantModelMode::SlowSlope&&isfinite(r.model.kPrime)&&r.model.kPrime>0&&slopePass&&
        std::fabs(r.model.kPrime-r.kTrue)/r.kTrue<=0.22&&std::fabs(r.model.thetaSec-dead)<=30.0&&r.model.confidence>=0.45;
      if(!pass)++fail;
      std::printf("%s,%u,%.2f,%+.6f,%+.6f,%s,%.8f,%.8f,%u,%.2f,%.3f,%s\n",plant.name,dead,resolution,slope,r.model.baselineSlope,positive?"POS":"NEG",
        r.kTrue,r.model.kPrime,dead,r.model.thetaSec,r.model.confidence,r.model.valid?(pass?"PASS":"OUT_OF_TOL"):thermalPlantIdReasonName(r.model.reason));
      equivalence[{plant.name,dead,(int)std::lround(resolution*100),(positive?1:-1)}].push_back({slope,r.model});
    }
  for(const auto &entry:equivalence){const auto &v=entry.second;assert(v.size()==3);double minK=1e9,maxK=-1,minTheta=1e9,maxTheta=-1;
    for(const auto &x:v){if(x.second.valid){minK=std::min(minK,(double)x.second.kPrime);maxK=std::max(maxK,(double)x.second.kPrime);minTheta=std::min(minTheta,(double)x.second.thetaSec);maxTheta=std::max(maxTheta,(double)x.second.thetaSec);}}
    if(!(minK>0&&maxK/minK<=1.20&&maxTheta-minTheta<=20.0)){++fail;}
  }
  // Noise-aware rejection: same heavy plant/step but trend obscured by large PV noise.
  const auto noisy=runCase(plants[2],60,0.01,0.0015,true,true);
  assert(!noisy.model.valid);
  // Insufficient trend change: 2 percentage-point actual step is below the evidence floor.
  const auto weak=runCase(plants[1],30,0.01,-0.0015,true,false,0.02);
  assert(!weak.model.valid);
  std::printf("TREND_SUMMARY total=%u failures=%u noisy=%s weak=%s\n",total,fail,
    thermalPlantIdReasonName(noisy.model.reason),thermalPlantIdReasonName(weak.model.reason));
  assert(total==144&&fail==0);
}
