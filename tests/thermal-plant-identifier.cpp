#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_plant_identifier.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

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
    const bool ssr=tick>=baselineTicks;const double actual=ssr?stepFraction:0.0;
    if(heaterLag>0)actuator+=(actual-actuator)*std::min(1.0,dt/heaterLag);else actuator=actual;
    const double delayed=delay[cursor];delay[cursor]=actuator;cursor=(cursor+1)%delay.size();
    temp+=(ambient+heaterWatts*delayed/plant.loss-temp)*dt/tau;
    const double sampled=std::round(temp/resolution)*resolution;
    assert(id.addSample(tick*5000U,static_cast<float>(filter.update(sampled)),static_cast<float>(actual)));
  }
  return id.identify();
}

static void invalidCases(){
  ThermalPlantIdentifier id;
  for(unsigned i=0;i<100;++i){
    assert(id.addSample(i*5000U,25.0f,0.0f));
  }
  assert(!id.identify().valid&&id.identify().reason==ThermalPlantIdReason::NoActualStep);
  id.reset();
  for(unsigned i=0;i<100;++i)assert(id.addSample(i*5000U,25.0f,i>=20?1.0f:0.0f));
  assert(!id.identify().valid&&id.identify().reason==ThermalPlantIdReason::NoResponse);
  id.reset();
  for(unsigned i=0;i<30;++i)assert(id.addSample(i*5000U,25.0f+(i>20?(i-20)*0.01f:0),i>=20?1.0f:0.0f));
  assert(!id.identify().valid&&id.identify().reason==ThermalPlantIdReason::InsufficientSamples);
  id.reset();
  for(unsigned i=0;i<100;++i)assert(id.addSample(i*5000U,25.0f+(i>20?(i-20)*0.02f:0),i>=20?(i%2?1.0f:0.0f):0.0f));
  assert(!id.identify().valid&&id.identify().reason==ThermalPlantIdReason::UnstableActualInput);
  id.reset();
  for(unsigned i=0;i<200;++i){const float n=static_cast<float>(std::sin(i*2.17)*0.8);
    const float weakResponse=i>=30?1.0f-expf(-static_cast<float>(i-30)*5.0f/80.0f):0.0f;
    assert(id.addSample(i*5000U,25.0f+weakResponse+n,i>=30?1.0f:0.0f));}
  assert(!id.identify().valid&&id.identify().reason==ThermalPlantIdReason::ExcessiveNoise);
}

int main(){
  const Plant plants[]={{"light",180000,120},{"medium",600000,180},{"heavy",1600000,300}};
  std::vector<Row> rows;unsigned failures=0;
  std::puts("plant,dead_s,resolution,heater_lag_s,K_true,K_est,tau_true,tau_est,theta_est,rmse,confidence,result");
  for(const auto &plant:plants)for(unsigned dead:{5U,15U,30U,60U})for(double resolution:{0.1,0.01})for(double lag:{0.0,8.0}){
    const auto model=identifyPlant(plant,dead,resolution,lag);const double kTrue=16000.0/plant.loss,tauTrue=plant.capacity/plant.loss;
    bool pass=model.valid;
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
  invalidCases();
  assert(rows.size()==48&&failures==0);
  std::puts("ThermalPlantIdentifier: 48 FOPDT ground-truth cases + 5 invalid-input cases PASS");
}
