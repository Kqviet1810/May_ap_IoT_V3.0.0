#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_plant_identifier.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
using std::isfinite;
#include "../MAYAP_INDUSTRIAL_v1_0_0/heater_burst_scheduler.h"

struct Plant { const char *name; double capacity,loss; };
struct SensorFilter {
  double window[3]{},filtered=0;unsigned count=0,index=0;bool ready=false;
  double update(double value){window[index]=value;index=(index+1)%3;if(count<3)++count;
    double sorted[3]{window[0],window[1],window[2]};std::sort(sorted,sorted+count);
    const double median=sorted[count/2];if(!ready){filtered=median;ready=true;}else filtered+=(median-filtered)*3.0/8.0;return filtered;}
};
struct Row { const char *plant;unsigned dead;double resolution,lag,kTrue,tauTrue,k,tau,theta,rmse,confidence; };

static ThermalPlantModel identifyPlant(const Plant &plant,unsigned dead,double resolution,double heaterLag){
  constexpr double dt=5.0,heaterWatts=16000.0,stepFraction=0.10,ambient=25.0;
  const double tau=plant.capacity/plant.loss;
  const unsigned baselineTicks=60,totalTicks=baselineTicks+static_cast<unsigned>((dead+6.0*tau)/dt);
  std::vector<double> delay(std::max(1U,static_cast<unsigned>(std::ceil(dead/dt))),0.0);
  size_t cursor=0;double actuator=0,temp=ambient;SensorFilter filter;ThermalPlantIdentifier id;
  for(unsigned tick=0;tick<=totalTicks;++tick){
    const bool step=tick>=baselineTicks;const double actual=step?stepFraction:0.0;
    if(heaterLag>0)actuator+=(actual-actuator)*std::min(1.0,dt/heaterLag);else actuator=actual;
    const double delayed=delay[cursor];delay[cursor]=actuator;cursor=(cursor+1)%delay.size();
    temp+=(ambient+heaterWatts*delayed/plant.loss-temp)*dt/tau;
    const double sampled=std::round(temp/resolution)*resolution;
    assert(id.addSample(tick*5000U,static_cast<float>(filter.update(sampled)),static_cast<float>(actual)));
  }
  return id.identify();
}

static ThermalPlantModel identifyEarly(const Plant &plant,unsigned dead,double observeSec,double resolution=0.01){
  constexpr double dt=5.0,heaterWatts=16000.0,stepFraction=0.10,ambient=25.0;
  const double tau=plant.capacity/plant.loss;
  const unsigned baselineTicks=60,totalTicks=baselineTicks+static_cast<unsigned>((dead+observeSec)/dt);
  std::vector<double> delay(std::max(1U,static_cast<unsigned>(std::ceil(dead/dt))),0.0);
  size_t cursor=0;double temp=ambient;SensorFilter filter;ThermalPlantIdentifier id;
  for(unsigned tick=0;tick<=totalTicks;++tick){
    const double actual=tick>=baselineTicks?stepFraction:0.0;
    const double delayed=delay[cursor];delay[cursor]=actual;cursor=(cursor+1)%delay.size();
    temp+=(ambient+heaterWatts*delayed/plant.loss-temp)*dt/tau;
    const double sampled=std::round(temp/resolution)*resolution;
    assert(id.addSample(tick*5000U,static_cast<float>(filter.update(sampled)),static_cast<float>(actual)));
  }
  return id.identify();
}

static void invalidCases(){
  ThermalPlantIdentifier id;
  for(unsigned i=0;i<100;++i) assert(id.addSample(i*5000U,25.0f,0.0f));
  assert(!id.identify().valid&&id.identify().reason==ThermalPlantIdReason::NoActualStep);
  id.reset();
  for(unsigned i=0;i<100;++i)assert(id.addSample(i*5000U,25.0f,i>=20?1.0f:0.0f));
  assert(!id.identify().valid&&id.identify().reason==ThermalPlantIdReason::NoResponse);
  id.reset();
  for(unsigned i=0;i<30;++i)assert(id.addSample(i*5000U,25.0f+(i>20?(i-20)*0.01f:0),i>=20?1.0f:0.0f));
  assert(!id.identify().valid&&id.identify().reason==ThermalPlantIdReason::InsufficientSamples);
  id.reset();
  // Alternating delivered fraction is genuinely unstable interval energy. Raw
  // SSR PDM is tested separately only after ON-time bucketing.
  for(unsigned i=0;i<100;++i)assert(id.addSample(i*5000U,25.0f+(i>20?(i-20)*0.02f:0),i>=20?(i%2?1.0f:0.0f):0.0f));
  assert(!id.identify().valid&&id.identify().reason==ThermalPlantIdReason::UnstableActualInput);
  id.reset();
  for(unsigned i=0;i<200;++i){const float n=static_cast<float>(std::sin(i*2.17)*0.8);
    const float weakResponse=i>=30?1.0f-expf(-static_cast<float>(i-30)*5.0f/80.0f):0.0f;
    assert(id.addSample(i*5000U,25.0f+weakResponse+n,i>=30?1.0f:0.0f));}
  assert(!id.identify().valid&&id.identify().reason==ThermalPlantIdReason::ExcessiveNoise);
}

static ThermalPlantModel identifyActualPdm(double requestedFraction,float &meanDelivered){
  constexpr uint32_t rawDtMs=100U,bucketMs=30000U;
  constexpr double ambient=25.0,heaterWatts=16000.0,capacity=180000.0,loss=120.0;
  constexpr uint32_t baselineMs=600000U;
  const double tau=capacity/loss;
  const uint32_t totalMs=baselineMs+static_cast<uint32_t>(6.0*tau*1000.0);
  HeaterBurstScheduler pdm(1U,300U);ThermalDeliveredEnergyBucket bucket;ThermalPlantIdentifier id;
  bool ssr=false;bucket.reset(0U,false);double temp=ambient;double deliveredSum=0;unsigned deliveredCount=0;
  for(uint32_t now=rawDtMs;now<=totalMs;now+=rawDtMs){
    const double req=now>baselineMs?requestedFraction*100.0:0.0;
    const auto demand=pdm.update(now,static_cast<float>(req),true);ssr=demand.groupA;
    assert(bucket.observe(now,ssr));
    temp+=(ambient+heaterWatts*(ssr?1.0:0.0)/loss-temp)*(rawDtMs*0.001)/tau;
    if(now%bucketMs==0U){
      float actualFraction=NAN;assert(bucket.close(now,actualFraction));
      // Only actual ON-time fraction enters the identifier. Requested duty is
      // intentionally absent from the identifier API.
      assert(id.addSample(now,static_cast<float>(temp),actualFraction));
      if(now>baselineMs){deliveredSum+=actualFraction;++deliveredCount;}
    }
  }
  meanDelivered=deliveredCount?static_cast<float>(deliveredSum/deliveredCount):NAN;
  return id.identify();
}

static void pdmCases(){
  for(double requested:{0.10,0.20,0.30}){
    float meanDelivered=NAN;const auto model=identifyActualPdm(requested,meanDelivered);
    std::printf("PDM %.0f%% actual=%.5f mode=%s result=%s K=%.3f tau=%.1f theta=%.1f\n",
      requested*100.0,meanDelivered,thermalPlantModelModeName(model.mode),thermalPlantIdReasonName(model.reason),
      model.processGain,model.tauSec,model.thetaSec);
    assert(std::fabs(meanDelivered-requested)<0.012);
    assert(model.reason!=ThermalPlantIdReason::UnstableActualInput);
    assert(model.valid&&model.mode==ThermalPlantModelMode::FOPDT);
  }
}

static void slowCases(){
  const Plant medium={"medium",600000,180},heavy={"heavy",1600000,300};
  const auto m=identifyEarly(medium,30,1200.0);
  const auto h=identifyEarly(heavy,60,1800.0);
  const double kmTrue=(16000.0/medium.loss)/(medium.capacity/medium.loss);
  const double khTrue=(16000.0/heavy.loss)/(heavy.capacity/heavy.loss);
  std::printf("SLOW medium mode=%s kPrime=%.8f true=%.8f theta=%.1f conf=%.3f rmse=%.4f\n",
    thermalPlantModelModeName(m.mode),m.kPrime,kmTrue,m.thetaSec,m.confidence,m.residualRmseC);
  std::printf("SLOW heavy  mode=%s kPrime=%.8f true=%.8f theta=%.1f conf=%.3f rmse=%.4f\n",
    thermalPlantModelModeName(h.mode),h.kPrime,khTrue,h.thetaSec,h.confidence,h.residualRmseC);
  assert(m.valid&&m.mode==ThermalPlantModelMode::SlowSlope);
  assert(h.valid&&h.mode==ThermalPlantModelMode::SlowSlope);
  assert(std::fabs(m.kPrime-kmTrue)/kmTrue<0.20&&std::fabs(m.thetaSec-30.0)<45.0);
  assert(std::fabs(h.kPrime-khTrue)/khTrue<0.20&&std::fabs(h.thetaSec-60.0)<60.0);
  assert(!std::isfinite(m.processGain)&&!std::isfinite(m.tauSec));
  assert(!std::isfinite(h.processGain)&&!std::isfinite(h.tauSec));
}

int main(){
  const Plant plants[]={{"light",180000,120},{"medium",600000,180},{"heavy",1600000,300}};
  std::vector<Row> rows;unsigned failures=0;
  std::printf("identifier_bytes=%zu\n",sizeof(ThermalPlantIdentifier));
  std::puts("plant,dead_s,resolution,heater_lag_s,K_true,K_est,tau_true,tau_est,theta_est,rmse,confidence,result");
  for(const auto &plant:plants)for(unsigned dead:{5U,15U,30U,60U})for(double resolution:{0.1,0.01})for(double lag:{0.0,8.0}){
    const auto model=identifyPlant(plant,dead,resolution,lag);const double kTrue=16000.0/plant.loss,tauTrue=plant.capacity/plant.loss;
    bool pass=model.valid&&model.mode==ThermalPlantModelMode::FOPDT;
    if(pass){pass=std::fabs(model.processGain-kTrue)/kTrue<=0.05&&std::fabs(model.tauSec-tauTrue)/tauTrue<=0.18&&
      std::fabs(model.thetaSec-dead)<=25.0&&model.normalizedResidual<=0.08&&model.confidence>=0.50;}
    if(!pass)++failures;
    rows.push_back({plant.name,dead,resolution,lag,kTrue,tauTrue,model.processGain,model.tauSec,model.thetaSec,model.residualRmseC,model.confidence});
    std::printf("%s,%u,%.2f,%.0f,%.4f,%.4f,%.1f,%.1f,%.1f,%.4f,%.3f,%s\n",plant.name,dead,resolution,lag,
      kTrue,model.processGain,tauTrue,model.tauSec,model.thetaSec,model.residualRmseC,model.confidence,
      model.valid?(pass?"PASS":"OUT_OF_TOLERANCE"):thermalPlantIdReasonName(model.reason));
  }
  for(const auto &plant:plants)for(double resolution:{0.1,0.01})for(double lag:{0.0,8.0}){
    double previous=-1;for(const auto &row:rows)if(std::string(row.plant)==plant.name&&row.resolution==resolution&&row.lag==lag){
      assert(row.theta>=previous);previous=row.theta;}
  }
  invalidCases();pdmCases();slowCases();
  assert(rows.size()==48&&failures==0);
  std::puts("ThermalPlantIdentifier: 48 FOPDT + actual-PDM 10/20/30 + slow-slope + invalid/noise PASS");
}
