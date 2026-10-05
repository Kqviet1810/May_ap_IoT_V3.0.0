// Deterministic plant using the extracted production MachineController heating
// route, ThermalController, production sensor filter, PDM and OutputArbiter.
// This is software evidence, never physical qualification of a 12 m3 chamber.
#include "thermal-heating-harness.h"
#include "actual-filter.inc"
#include "actual-safety-thresholds.inc"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
struct Case {
  double sp, ambient, capacity, loss, lag, effective, resolution, bias;
  unsigned delay;
  const char *event;
};
static void run(const Case &c) {
  clockMs=0;
  std::fill(levels,levels+64,LOW);
  bootReady=true;trip=maintenance=false;
  Harness h;h.outputs_.begin();h.config_.targetTemp=static_cast<float>(c.sp);
  ActualSensorFilter filter;
  constexpr unsigned stepMs=100, durationS=10800;
  constexpr double dt=stepMs/1000.0;
  std::vector<double> pipe(c.delay*1000/stepMs+1,0.0);
  size_t cursor=0;double temp=c.ambient, heater=0, target=c.sp;
  double peak=-1000, mae=0, p95=0, lo=1000, hi=-1000, energy=0;
  std::vector<double> tail;
  int high=0,emergency=0,settled=-1, lastOut=0;
  bool prevHigh=false,prevEmergency=false, everOn=false,cutSeen=false;
  double held=temp;unsigned samples=0;
  for(unsigned tick=0;tick<durationS*1000/stepMs;++tick){
    const unsigned seconds=tick*stepMs/1000;
    const uint32_t now=1000+tick*stepMs;
    const bool event=seconds>=3600;
    double ambient=c.ambient,loss=c.loss,cap=c.capacity;
    if(std::strcmp(c.event,"ambient_rise")==0 && event)ambient+=3;
    if(std::strcmp(c.event,"loss_up")==0 && event)loss*=1.6;
    if(std::strcmp(c.event,"loss_down")==0 && event)loss*=0.6;
    if(std::strcmp(c.event,"load_heavy")==0 && event)cap*=2;
    if(std::strcmp(c.event,"load_light")==0 && event)cap*=0.5;
    if(std::strcmp(c.event,"vent")==0 && event && seconds<4200)loss+=200;
    if(std::strcmp(c.event,"setpoint_step")==0)target=event?c.sp:30;
    if(tick==3600*1000/stepMs){
      if(std::strncmp(c.event,"door",4)==0)temp-=std::atof(c.event+4);
    }
    const bool lossSensor=std::strcmp(c.event,"sensor_loss")==0 && event && seconds<3660;
    const bool frozen=std::strcmp(c.event,"sensor_frozen")==0 && event && seconds<3670;
    const bool bad=std::strcmp(c.event,"bad_sample")==0 && tick==3600*1000/stepMs;
    const bool cut=std::strcmp(c.event,"safety_cut")==0 && event && seconds<3660;
    const bool power=std::strcmp(c.event,"power_recovery")==0 && event && seconds<3720;
    const bool jitter=std::strcmp(c.event,"jitter_drop")==0;
    const bool sampleTick=jitter ? tick%20==(seconds/7)%3 && (seconds/2)%11!=0 : tick%20==0;
    if(sampleTick){
      if(!lossSensor && !bad && !frozen){
        const double noise=std::strcmp(c.event,"noise")==0 ? .05*std::sin(seconds*.73)+.02*std::sin(seconds*.13):0;
        const float sampled=static_cast<float>(std::round((temp+c.bias+noise)/c.resolution)*c.resolution);
        filter.updateFilter(sampled,60);held=filter.value();++samples;
      }
      if(bad){h.sensorUsable_=false;cutSeen=true;}
      else h.sensorUsable_=!lossSensor && !frozen && samples>=6;
    }
    if(lossSensor||frozen)h.sensorUsable_=false;
    h.highTemperatureActive_=temp>=MODEL_HIGHTEMPALARM;
    h.emergencyActive_=temp>=MODEL_EMERGENCYTEMP;
    h.faults_.inhibit=cut||power||h.highTemperatureActive_||h.emergencyActive_;
    h.faults_.drop=h.highTemperatureActive_||h.emergencyActive_;
    h.config_.targetTemp=static_cast<float>(target);
    h.temperature_=static_cast<float>(held);
    h.cycle(now,sampleTick && h.sensorUsable_);
    const auto state=h.outputs_.state();
    const bool on=state.heaterSsr && state.heatMaster;
    if((cut||lossSensor||frozen||bad||power||h.highTemperatureActive_||h.emergencyActive_)&&on){
      std::fprintf(stderr,"unsafe heat during cut sp=%.1f ambient=%.1f dead=%u event=%s time=%.1f high=%u emergency=%u sensor=%u inhibit=%u\n",
          c.sp,c.ambient,c.delay,c.event,tick*dt,h.highTemperatureActive_,h.emergencyActive_,
          h.sensorUsable_,h.faults_.inhibit);std::abort();}
    if(on)everOn=true;
    if(cut||lossSensor||frozen||bad||power)cutSeen=true;
    const double delivered=on?16000*c.effective:0;
    const double delayed=pipe[cursor];pipe[cursor]=delivered;cursor=(cursor+1)%pipe.size();
    heater+=(delayed-heater)*dt/c.lag;
    temp+=(heater-loss*(temp-ambient))*dt/cap;
    energy+=delivered*dt;
    if(!std::isfinite(temp)||!std::isfinite(h.runtime_.heaterPower)||h.runtime_.heaterPower<0||h.runtime_.heaterPower>100){
      std::fprintf(stderr,"nonfinite/unbounded thermal state\n");std::abort();
    }
    const bool crossingHigh=temp>=MODEL_HIGHTEMPALARM;
    const bool crossingEmergency=temp>=MODEL_EMERGENCYTEMP;
    if(crossingHigh&&!prevHigh)++high;
    if(crossingEmergency&&!prevEmergency)++emergency;
    prevHigh=crossingHigh;prevEmergency=crossingEmergency;
    if(std::strcmp(c.event,"setpoint_step")!=0||event){
      peak=std::max(peak,temp-target);
      if(std::fabs(temp-target)>.15)lastOut=seconds;
    }
    if(seconds>=durationS-1800 && tick%10==0){
      const double e=temp-target;tail.push_back(std::fabs(e));
      mae+=std::fabs(e);lo=std::min(lo,temp);hi=std::max(hi,temp);
    }
  }
  std::sort(tail.begin(),tail.end());mae/=tail.size();p95=tail[static_cast<size_t>(.95*(tail.size()-1))];
  settled=lastOut<static_cast<int>(durationS-1800)?lastOut+1:-1;
  const bool pass=high==0&&emergency==0&&peak<=.3&&mae<=.1&&p95<=.15&&hi-lo<=.25&&settled>=0;
  std::printf("%.1f,%.1f,%.0f,%.0f,%u,%.1f,%.2f,%.2f,%.2f,%s,%s,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%.0f,%s\n",
      c.sp,c.ambient,c.capacity,c.loss,c.delay,c.lag,c.effective,c.resolution,c.bias,c.event,
#ifdef THERMAL_V3_BASELINE
      "BASELINE",
#else
      "PHASE1",
#endif
      peak,mae,p95,hi-lo,settled,high,emergency,energy,pass?"PASS":"FAIL");
  const bool expectsCut=std::strcmp(c.event,"sensor_loss")==0 ||
      std::strcmp(c.event,"bad_sample")==0 ||
      std::strcmp(c.event,"sensor_frozen")==0 ||
      std::strcmp(c.event,"safety_cut")==0 ||
      std::strcmp(c.event,"power_recovery")==0;
  if(!((everOn || c.ambient>=c.sp) && (!expectsCut || cutSeen))) {
    std::fprintf(stderr,"missing heat or cut sp=%.1f ambient=%.1f dead=%u event=%s on=%u cut=%u\n",
        c.sp,c.ambient,c.delay,c.event,everOn,cutSeen);std::abort();
  }
}
int main(){
  std::puts("sp,ambient,capacity,loss,delay,lag,effective,resolution,bias,event,controller,overshoot,mae,p95,ripple,settling,high,emergency,delivered_j,target");
  const double capacities[]={180000,600000,1600000};
  const unsigned delays[]={0,5,15,30,60,120};
  const double ambients[]={10,20,25,28,35};
  for(double sp:{30.,32.,35.,37.5})for(unsigned ai=0;ai<5;++ai)for(unsigned ci=0;ci<3;++ci)
    for(unsigned di=0;di<6;++di)for(unsigned ri=0;ri<2;++ri){
      const unsigned k=ai*36+ci*12+di*2+ri;
      run({sp,ambients[ai],capacities[ci],ci==0?120.:ci==1?180.:300.,
           k%3==0?3.:k%3==1?8.:30.,k%3==0?.7:k%3==1?1.:1.2,ri==0?.1:.01,
           k%7==0?.08:0.,delays[di],"cold"});
    }
  for(const char *event:{"door1","door2","door5","ambient_rise","loss_up","loss_down",
      "load_heavy","load_light","vent","sensor_loss","bad_sample","sensor_frozen",
      "safety_cut","power_recovery","setpoint_step","noise","jitter_drop"})
    for(unsigned di:{0U,15U,60U,120U})
      run({37.5,25,di%2?600000.:180000.,di%2?180.:120.,di%2?8.:30.,1.,.01,0.,di,event});
}
