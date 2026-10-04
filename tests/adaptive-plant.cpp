// Uncalibrated whole-chamber model. Cooling capacity is UNKNOWN: zero cooling
// watts are credited, even when the real ON/OFF fan request is issued.
#include "../MAYAP_INDUSTRIAL_v1_0_0/adaptive_thermal_balance.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/adaptive_persistence.h"
#define MAYAP_TEST_ADAPTIVE 1
#define MAYAP_SENSOR_PROFILE 0
#define MAYAP_ADAPTIVE_OBSERVER_ONLY 0
#include "thermal-autotune-harness.h"
#include "actual-filter.inc"
#include <fstream>
#include <iomanip>
#include <iostream>
struct Plant{const char *name;double capacity,loss;};
struct ModelPlant {
  Plant plant;double ambient,temp,watts=0,internal=0;std::vector<double> delay;size_t cursor=0;
  ModelPlant(Plant p,double a,unsigned dead):plant(p),ambient(a),temp(a),delay(dead*10,0){}
  void step(bool on){
    const double delayed=delay[cursor];delay[cursor]=on?16000:0;cursor=(cursor+1)%delay.size();
    watts+=(delayed-watts)*.1/8;temp+=(watts+internal-plant.loss*(temp-ambient))*.1/plant.capacity;
    assert(std::isfinite(temp));
  }
};
struct Metrics{double mae=0,p95=0,ripple=0,overshoot=0,energy=0,maxSlew=0,normalSlew=0,confidence=0;
  int settling=-1;unsigned transitions=0,ventTicks=0,falseLearn=0,selfTicks=0,adaptiveTicks=0;
  bool high=false,emergency=false;};
static Metrics run(Plant plant,double ambient,unsigned dead,double resolution,float sp,
                   const std::string &scenario,bool enabled,std::ofstream &observer,std::ofstream &control,bool trace){
  ModelPlant m(plant,ambient,dead);TuneHarness h;h.batchRunning_=true;
  h.config_.targetTemp=sp;h.config_.adaptiveThermalBalanceEnabled=enabled;
  ActualSensorFilter filter;Metrics result;bool last=false;double lastLimit=100;
  std::vector<double> errors,tail;int lastOutside=0;
  auto lastState=MayapAdaptive::State::Disabled;
  for(unsigned tick=0;tick<108000;++tick){
    const double t=tick*.1;const uint32_t now=1000+tick*100;
    if(tick==54000){
      if(scenario=="light_to_heavy")m.plant.capacity=1200000;
      if(scenario=="heavy_to_light")m.plant.capacity=150000;
      if(scenario=="loss_up")m.plant.loss=plant.loss*1.5;
      if(scenario=="loss_down")m.plant.loss=plant.loss*.6;
      if(scenario=="ambient_rise")m.ambient+=3;
      if(scenario=="door")m.temp-=2;
      if(scenario=="setpoint")h.config_.targetTemp-=2;
      if(scenario=="power_recovery"){h.adaptiveThermal_.reset();h.heaterBurst_.reset();h.pid_.reset();}
    }
    if(scenario=="self_heating")m.internal=t<2700?0:t<5400?500:t<8100?1000:2000;
    h.sensorUsable_=!(scenario=="sensor_loss"&&t>=5400&&t<5420);
    h.abnormalResetLatched_=scenario=="power_recovery"&&t>=5400&&t<5580;
    h.testModeActive_=scenario=="test_mode"&&t>=5400&&t<5460;
    h.faults_.inhibit=scenario=="safety_cut"&&t>=5400&&t<5460;
    h.config_.ventAutoEnabled=scenario=="scheduled_vent"&&t>=5400&&t<5460;
    h.config_.ventDutyDay1To3=90;h.config_.ventCycleMinutes=40;
    // Same conservative physical crossing qualification used in prior matrices;
    // actual alarm confirmation timers/vent heat removal are not simulated.
    h.highTemperatureActive_=m.temp>=h.config_.highTempAlarm;
    h.emergencyActive_=m.temp>=h.config_.emergencyTemp;
    h.faults_.cooling=h.highTemperatureActive_||h.emergencyActive_;
    h.faults_.inhibit=h.faults_.inhibit||h.highTemperatureActive_||h.emergencyActive_;
    const bool sample=tick%20==0;
    if(sample){
      filter.updateFilter(static_cast<float>(std::round(m.temp/resolution)*resolution),60);
      if(scenario=="bad_sample"&&tick==54000)h.sample(NAN,NAN);
      else h.sample(static_cast<float>(m.temp),filter.value());
    }
    const uint32_t previous=h.adaptiveThermal_.observer().estimates().windows;
    h.cycle(now,sample);
    const auto &e=h.adaptiveThermal_.observer().estimates();const auto &d=h.adaptiveThermal_.decision();
    assert(std::isfinite(d.effective)&&d.effective>=0&&d.effective<=h.config_.maxHeaterPower);
    assert(std::isfinite(h.runtime_.heaterPower)&&h.runtime_.heaterPower>=0&&h.runtime_.heaterPower<=h.config_.maxHeaterPower);
    if(e.windows!=previous&&!e.learningValid){
      std::cerr<<"adaptive invalid window "<<plant.name<<" "<<scenario<<" t="<<t
               <<" enabled="<<enabled<<" energy="<<result.energy
               <<" high="<<h.highTemperatureActive_<<" emergency="<<h.emergencyActive_
               <<" sensor="<<h.sensorUsable_<<" test="<<h.testModeActive_
               <<" state="<<static_cast<int>(d.state)<<" reason="<<static_cast<int>(d.reason)
               <<" prev="<<previous<<" now="<<e.windows<<"\n";
      ++result.falseLearn;assert(false);
    }
    if(h.highTemperatureActive_||h.emergencyActive_||h.faults_.inhibit||!h.sensorUsable_||h.abnormalResetLatched_)
      assert(!h.outputs_.state().heaterSsr);
    const bool on=h.outputs_.state().heaterSsr;
    if(on!=last)++result.transitions;
    last=on;
    if(on)result.energy+=1600;
    if(h.outputs_.state().ventFan)++result.ventTicks;
    if(d.selfHeating)++result.selfTicks;
    if(d.state==MayapAdaptive::State::Adaptive)++result.adaptiveTicks;
    const double slew=std::fabs(d.effective-lastLimit)*600;
    result.maxSlew=std::max(result.maxSlew,slew);
    const bool exception=d.state==MayapAdaptive::State::FaultBypass ||
      lastState==MayapAdaptive::State::FaultBypass || d.state==MayapAdaptive::State::SelfHeating ||
      (scenario=="setpoint"&&tick==54000) || (scenario=="power_recovery"&&tick==54000);
    if(enabled&&!exception){
      result.normalSlew=std::max(result.normalSlew,slew);
      assert(d.effective-lastLimit<=MayapAdaptive::Policy::RisePctPerMin/600+.00002);
      assert(lastLimit-d.effective<=MayapAdaptive::Policy::FallPctPerMin/600+.00002);
    }
    lastLimit=d.effective;lastState=d.state;
    m.step(on);
    result.high=result.high||m.temp>=h.config_.highTempAlarm;result.emergency=result.emergency||m.temp>=h.config_.emergencyTemp;
    result.overshoot=std::max(result.overshoot,m.temp-h.config_.targetTemp);
    if(std::fabs(m.temp-h.config_.targetTemp)>.15)lastOutside=t;
    if(t>=10200){errors.push_back(std::fabs(m.temp-h.config_.targetTemp));tail.push_back(m.temp);}
    if(trace&&tick%600==0){
      const std::string prefix=std::string(plant.name)+","+scenario+","+(enabled?"ADAPTIVE":"BASELINE")+","+std::to_string(t)+",";
      observer<<prefix<<m.temp<<','<<h.config_.targetTemp<<','<<on<<','<<h.runtime_.heaterPower<<','<<d.effective<<','<<e.load<<','<<e.coast<<','<<e.hold<<','<<e.confidence<<','<<MayapAdaptive::stateName(d.state)<<','<<d.selfHeating<<','<<d.coolingDemand<<','<<e.windows<<','<<e.learningValid<<','<<e.rate<<','<<d.fastPredictedPeak<<','<<d.fastPath<<'\n';
      control<<prefix<<m.temp<<','<<h.config_.targetTemp<<','<<on<<','<<h.runtime_.heaterPower<<','<<d.effective<<','<<e.load<<','<<e.coast<<','<<e.hold<<','<<h.adaptiveThermal_.effectiveConfidence()<<','<<MayapAdaptive::stateName(d.state)<<','<<d.selfHeating<<','<<d.coolingDemand<<','<<h.outputs_.state().ventFan<<','<<h.highTemperatureActive_<<','<<h.emergencyActive_<<'\n';
    }
  }
  result.settling=lastOutside<10200?lastOutside:-1;result.confidence=h.adaptiveThermal_.effectiveConfidence();
  for(double error:errors)result.mae+=error/errors.size();
  std::sort(errors.begin(),errors.end());result.p95=errors[errors.size()*95/100];
  result.ripple=*std::max_element(tail.begin(),tail.end())-*std::min_element(tail.begin(),tail.end());
  return result;
}
int main(int argc,char **argv){
  assert(argc==3);std::ofstream observer(argv[1]),control(argv[2]);
  observer<<"plant,scenario,mode,time,PV,SP,actualHeater,requestedPower,effectiveMax,loadIndex,coast,holdPower,confidence,adaptiveState,selfHeating,coolingDemand,validWindows,learningValid,rate,predictedPeak,fastPath\n";
  control<<"plant,scenario,mode,time,PV,SP,actualHeater,requestedPower,effectiveMax,loadIndex,coast,holdPower,confidence,adaptiveState,selfHeating,coolingDemand,actualVent,High,Emergency\n";
  std::cout<<"plant,scenario,mode,ambient,deadtime,resolution,SP,MAE,P95,ripple,overshoot,settling,High,Emergency,energy_j,heater_transitions,vent_runtime_s,max_authority_slew_pct_min,normal_authority_slew_pct_min,confidence_final,false_learning_count,self_heating_s,adaptive_s,cooling_capacity_w\n"<<std::fixed<<std::setprecision(6);
#include "actual-plants.inc"
  for(const Plant &p:plants)for(const std::string scenario:{"steady","light_to_heavy","heavy_to_light","loss_up","loss_down","ambient_rise","door","self_heating","sensor_loss","bad_sample","safety_cut","scheduled_vent","setpoint","power_recovery","test_mode"}){
    for(double ambient:{20.,25.,28.})for(unsigned dead:{5U,15U,30U,60U})for(double resolution:{.1,.01})for(float sp:{30.f,32.f,35.f,37.5f}){
      if(scenario!="steady"&&(ambient!=25||sp!=37.5f))continue;
      for(bool enabled:{false,true}){
        const bool trace=(ambient==25&&dead==15&&resolution==.1&&sp==37.5f)||
          (std::string(p.name)=="medium"&&scenario=="heavy_to_light"&&ambient==25&&dead==60&&resolution==.1&&sp==37.5f);
        const auto m=run(p,ambient,dead,resolution,sp,scenario,enabled,observer,control,trace);
        std::cout<<p.name<<','<<scenario<<','<<(enabled?"ADAPTIVE":"BASELINE")<<','<<ambient<<','<<dead<<','<<resolution<<','<<sp<<','<<m.mae<<','<<m.p95<<','<<m.ripple<<','<<m.overshoot<<','<<m.settling<<','<<m.high<<','<<m.emergency<<','<<m.energy<<','<<m.transitions<<','<<m.ventTicks*.1<<','<<m.maxSlew<<','<<m.normalSlew<<','<<m.confidence<<','<<m.falseLearn<<','<<m.selfTicks*.1<<','<<m.adaptiveTicks*.1<<",0\n";
      }
    }
  }
}
