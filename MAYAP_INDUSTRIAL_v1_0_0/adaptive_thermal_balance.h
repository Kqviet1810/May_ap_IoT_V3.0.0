#pragma once
#include "thermal_observer.h"

namespace MayapAdaptive {
enum class State:uint8_t {Disabled,Learning,Qualified,Adaptive,SelfHeating,Degraded,FaultBypass};
enum class Reason:uint8_t {Disabled,Learning,Qualified,Limited,InvalidWindow,SensorSafety,ConfigChanged,SelfHeating,Cooling,SeedInvalid};
inline const char *stateName(State state){
  static const char *const names[]={"Disabled","Learning","Qualified","Adaptive","SelfHeating","Degraded","FaultBypass"};
  return names[static_cast<uint8_t>(state)];
}
struct Decision {
  State state=State::Disabled;Reason reason=Reason::Disabled;
  float effective=100,approach=0,coolingDemand=0;bool cooling=false,selfHeating=false;
};
class AdaptiveThermalSupervisor {
 public:
  void reset(){*this=AdaptiveThermalSupervisor{};}
  void invalidate(){observer_.degrade();qualificationAnchor_=observer_.estimates().windows;d_.reason=Reason::ConfigChanged;ready_=false;}
  ThermalObserver &observer(){return observer_;}
  const ThermalObserver &observer()const{return observer_;}
  const Decision &decision()const{return d_;}
  float effectiveConfidence()const{return self_||cool_?Policy::HighConfidence:observer_.estimates().confidence;}
  bool changedEnabled()const{return changedEnabled_;}
  void tick(uint32_t now,bool actualOn){observer_.tick(now,actualOn);}
  void sample(uint32_t now,const Observation &o){observer_.sample(now,o);}
  Decision update(uint32_t now,bool enabled,float configured,const Observation &o,bool readOnly=false){
    changedEnabled_=enabled!=enabled_;const uint32_t dt=seen_?since(now,at_):0;seen_=true;at_=now;
    if(changedEnabled_){observer_.reset();ready_=false;enabled_=enabled;self_=confirm_=false;cool_=false;coolAt_=now;}
    if(!enabled){d_=Decision{};d_.effective=configured;return d_;}
    if(!std::isfinite(configured)||configured<0||configured>100||!o.sensor||
       !std::isfinite(o.pv)||!std::isfinite(o.raw)||o.safety||o.tune||o.test||o.maintenance||o.recovery){
      observer_.suspend(!o.sensor||o.safety||!std::isfinite(o.pv)||!std::isfinite(o.raw));
      d_=Decision{};d_.state=State::FaultBypass;d_.reason=Reason::SensorSafety;
      d_.effective=std::isfinite(configured)?bound(configured,0,100):0;ready_=false;
      qualificationAnchor_=observer_.estimates().windows;
      self_=confirm_=cool_=false;coolAt_=now;return d_;
    }
    const auto &e=observer_.estimates();
    if(!ThermalObserver::finiteSeed(e.load,e.coast,e.coastSec,e.hold)||
       !std::isfinite(e.confidence)||!std::isfinite(e.rate)){
      observer_.reset();d_=Decision{};d_.effective=configured;d_.state=State::FaultBypass;
      d_.reason=Reason::InvalidWindow;self_=confirm_=cool_=ready_=false;return d_;
    }
    if(readOnly){d_=Decision{};d_.state=State::Learning;d_.reason=Reason::Learning;
      d_.effective=configured;self_=confirm_=cool_=ready_=false;return d_;}
    const bool positive=e.learningValid&&o.fanStable&&!o.vent&&observer_.offMs(now)>=Policy::CoastMs&&
      o.pv>o.sp+Policy::SelfOnC&&e.rate>Policy::SelfRateCPerSec;
    if(positive&&!confirm_){confirm_=true;confirmAt_=now;}
    if(!positive)confirm_=false;
    if(confirm_&&since(now,confirmAt_)>=Policy::SelfConfirmMs)self_=true;
    if(self_&&o.pv<=o.sp+Policy::SelfOffC)self_=false;
    const bool wantCool=self_&&o.pv>o.sp+Policy::SelfOnC;
    if(wantCool!=cool_&&since(now,coolAt_)>=Policy::CoolingMinMs){cool_=wantCool;coolAt_=now;}
    if(self_||cool_){
      d_.state=State::SelfHeating;d_.reason=Reason::SelfHeating;d_.selfHeating=self_;
      d_.cooling=cool_;d_.coolingDemand=cool_?100:0;d_.effective=0;ready_=true;return d_;
    }
    if(!e.learningValid)qualificationAnchor_=e.windows;
    if(!e.learningValid || e.confidence<Policy::MediumConfidence || e.validMs<Policy::LearnMinMs || e.windows-qualificationAnchor_<Policy::MinQualifiedWindows){
      const float before=d_.effective;
      d_=Decision{};d_.state=e.windows?State::Degraded:State::Learning;
      d_.reason=e.learningValid?Reason::Learning:Reason::InvalidWindow;
      d_.effective=ready_?std::min(configured,before+Policy::RisePctPerMin*std::min<uint32_t>(dt,1000)*.001f/60):configured;
      return d_;
    }
    const bool high=e.confidence>=Policy::HighConfidence && e.coastWindows>=2;
    const float floor=std::min(configured,std::max(Policy::MinAuthorityPct,e.hold*1.5f+10));
    // Medium evidence may reduce at most 20%. High evidence allows a wider
    // envelope; this is an authority ceiling, never a second PID demand.
    float target=std::max(floor,configured*(high?(0.5f+0.5f*e.load):0.8f));
    d_.approach=bound(e.coast+std::max(0.0f,e.rate)*e.coastSec,Policy::ApproachMinC,Policy::ApproachMaxC);
    if(o.pv>o.sp-d_.approach && e.rate>0){
      const float remaining=bound((o.sp-o.pv)/d_.approach,0,1);
      target=std::max(floor,target*(high?(0.5f+0.5f*remaining):(0.8f+0.2f*remaining)));
    }
    if(!ready_){d_.effective=configured;ready_=true;d_.state=State::Qualified;}
    const float delta=std::min<uint32_t>(dt,1000)*.001f/60;
    d_.effective=bound(target,d_.effective-Policy::FallPctPerMin*delta,d_.effective+Policy::RisePctPerMin*delta);
    d_.effective=bound(d_.effective,0,configured);d_.coolingDemand=0;d_.cooling=d_.selfHeating=false;
    d_.state=d_.effective<configured-0.01f?State::Adaptive:State::Qualified;
    d_.reason=d_.state==State::Adaptive?Reason::Limited:Reason::Qualified;return d_;
  }
 private:
  ThermalObserver observer_;Decision d_{};uint32_t at_=0;
  uint32_t confirmAt_=0,coolAt_=0;bool self_=false,confirm_=false,cool_=false;
  uint32_t qualificationAnchor_=0;
  bool enabled_=false,seen_=false,ready_=false,changedEnabled_=false;
};
}
