// Deterministic commissioning model, NOT proof of sensor accuracy/chamber uniformity.
// OLD is frozen main 9569fcc. Both paths share plant/noise/quantization/production filter.
#include "thermal-fixture.h"
#include <vector>
#include <string>
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_control.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/heater_burst_scheduler.h"
namespace Old {
#include "fixtures/thermal-v1/thermal_control.h"
#include "fixtures/thermal-v1/ssr_window.h"
}
#include "actual-filter.inc"
struct Plant { const char *name; double capacity; double loss; };
static void run(const Plant &plant,double sp,double ambient,unsigned delay,double resolution,
                const char *scenario,bool legacy,float beta,unsigned groups) {
  MachineConfig cfg;
  // The current MachineConfig defaults, verified against config.h by the driver.
  cfg.kp=18;cfg.ki=0.8f;cfg.kd=45;
  ThermalController pid(beta);Old::ThermalController oldPid;Old::LegacySsrWindow window;
  HeaterBurstScheduler burst(groups);
  ActualSensorFilter filter;
  constexpr double dt=0.25, duration=10800;
  std::vector<double> delayLine(static_cast<size_t>(delay/dt),0);
  size_t cursor=0;
  double temperature=ambient,heaterWatts=0,power=0;
  double overshoot=0,maxError=0,tailEnergy=0,totalEnergy=0;
  double target=sp,held=ambient;
  std::vector<double> tailErrors,tailTemperature;
  int lastOutside=0, recoverySamples=0;
  bool wasValid=false;
  unsigned cuts=0;
  const std::string event(scenario);
  for(unsigned tick=0;tick<static_cast<unsigned>(duration/dt);++tick) {
    const double t=tick*dt;
    const uint32_t ms=static_cast<uint32_t>(t*1000)+1000;
    double currentAmbient=ambient,loss=plant.loss;
    if(event=="setpoint_step") target=t<3600 ? 30 : sp;
    if(event=="ambient_rise" && t>=3600) currentAmbient+=3;
    if(event=="vent" && t>=3600 && t<4200) loss+=200;
    if(event=="door" && tick==14400) temperature-=3;
    const bool lost=event=="sensor_loss" && t>=3600 && t<3660;
    const bool bad=event=="one_bad_sample" && tick==14400;
    const bool safetyCut=event=="safety_cut" && t>=3600 && t<3660;
    if(tick%8==0) {
      // Equal deterministic noise on OLD and NEW: no random seed search.
      const double noise=event=="noise" ? 0.05*std::sin(t*0.73)+0.02*std::sin(t*0.13) : 0;
      if(lost || bad) { wasValid=false;recoverySamples=3;++cuts; }
      else {
        const float sampled=static_cast<float>(std::round((temperature+noise)/resolution)*resolution);
        filter.updateFilter(sampled,60);
        held=filter.value();
        if(recoverySamples) --recoverySamples;
        wasValid=t>=12 && recoverySamples==0; // Common startup/recovery gate for fair loop comparison.
      }
      if(wasValid && !safetyCut) power=legacy ? oldPid.updateOnNewSample(ms,target,held,cfg,true)
                                              : pid.updateOnNewSample(ms,target,held,cfg,true);
    }
    const bool permit=wasValid && !lost && !safetyCut;
    if(!permit) { power=0;pid.reset();oldPid.reset(); }
    bool a=false,b=false;
    if(legacy) a=permit && window.ssrWindowOn(ms,static_cast<float>(power),10);
    else { const auto d=burst.update(ms,static_cast<float>(power),permit);a=d.groupA;b=d.groupB; }
    // Energy delivered by the actuator, not requested PID %. OLD SSR is the full 16 kW bank.
    const double delivered=legacy ? (a?16000:0) : 16000.0*(a+b)/groups;
    const double delayed=delayLine[cursor];delayLine[cursor]=delivered;cursor=(cursor+1)%delayLine.size();
    // Same 8 s heater/air lag and first-order chamber energy/loss on every algorithm.
    heaterWatts+=(delayed-heaterWatts)*dt/8.0;
    temperature+=(heaterWatts-loss*(temperature-currentAmbient))*dt/plant.capacity;
    assert(std::isfinite(temperature) && std::isfinite(power) && power>=0 && power<=100);
    const double error=temperature-target;
    if(event!="setpoint_step" || t>=3600) {
      overshoot=std::max(overshoot,error);
      maxError=std::max(maxError,std::fabs(error));
      if(std::fabs(error)>0.15) lastOutside=static_cast<int>(t);
    }
    totalEnergy+=delivered*dt;
    if(t>=duration-1800) {
      tailEnergy+=delivered*dt;
      if(tick%4==0) { tailErrors.push_back(error);tailTemperature.push_back(temperature); }
    }
  }
  double mae=0,bias=0;std::vector<double> absolute;
  for(double e:tailErrors) { mae+=std::fabs(e);bias+=e;absolute.push_back(std::fabs(e)); }
  mae/=tailErrors.size();bias/=tailErrors.size();std::sort(absolute.begin(),absolute.end());
  const double p95=absolute[static_cast<size_t>(0.95*(absolute.size()-1))];
  const double ripple=*std::max_element(tailTemperature.begin(),tailTemperature.end())-*std::min_element(tailTemperature.begin(),tailTemperature.end());
  const int settling=lastOutside<9000 ? lastOutside+1-(event=="setpoint_step"?3600:0) : -1;
  const bool targetPass=mae<=0.1 && p95<=0.15 && ripple<=0.25 && overshoot<=0.3 && settling>=0;
  std::printf("%s,%s,%.1f,%.1f,%u,%.2f,%s,%.2f,%u,%.6f,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%u,%s\n",
      plant.name,scenario,sp,ambient,delay,resolution,legacy?"OLD":"NEW",beta,legacy?1:groups,
      overshoot,settling,mae,bias,p95,ripple,maxError,tailEnergy/(1800*16000)*100,totalEnergy/(duration*16000)*100,cuts,targetPass?"PASS":"FAIL");
  assert(totalEnergy>=0);
}
int main() {
  const Plant plants[]={{"light",180000,120},{"medium",600000,180},{"heavy",1600000,300}};
  std::puts("plant,scenario,setpoint,ambient,dead_s,resolution,algorithm,beta,groups,overshoot,settling_s,mean_abs_error,bias,p95_error,ripple,max_error,delivered_tail_pct,delivered_total_pct,sensor_fault_polls,targets");
  for(const auto &p:plants) for(double sp:{30.,32.,35.,37.5}) for(double ambient:{20.,25.,28.})
    for(unsigned delay:{5U,15U,30U,60U}) for(double resolution:{0.1,0.01}) {
      run(p,sp,ambient,delay,resolution,"cold_start",true,1,1);
      for(float beta:{1.f,0.7f,0.5f,0.35f}) for(unsigned groups:{1U,2U})
        run(p,sp,ambient,delay,resolution,"cold_start",false,beta,groups);
    }
  for(const auto &p:plants) for(unsigned delay:{15U,60U}) for(double resolution:{0.1,0.01})
    for(const char *scenario:{"vent","door","ambient_rise","noise","one_bad_sample","sensor_loss","safety_cut","setpoint_step"}) {
      run(p,37.5,25,delay,resolution,scenario,true,1,1);
      for(float beta:{1.f,0.7f,0.5f,0.35f}) for(unsigned groups:{1U,2U})
        run(p,37.5,25,delay,resolution,scenario,false,beta,groups);
    }
}
