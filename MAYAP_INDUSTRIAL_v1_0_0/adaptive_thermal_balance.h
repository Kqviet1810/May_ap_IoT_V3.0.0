#pragma once
#include "thermal_observer.h"

namespace MayapAdaptive {
enum class State:uint8_t {Disabled,Learning,Qualified,Adaptive,SelfHeating,Degraded,FaultBypass};
enum class Reason:uint8_t {Disabled,Learning,Qualified,Limited,InvalidWindow,SensorSafety,ConfigChanged,SelfHeating,Cooling,SeedInvalid,FastPath};
inline const char *stateName(State state){
  static const char *const names[]={"Disabled","Learning","Qualified","Adaptive","SelfHeating","Degraded","FaultBypass"};
  return names[static_cast<uint8_t>(state)];
}
struct Decision {
  State state=State::Disabled;Reason reason=Reason::Disabled;
  float effective=100,approach=0,coolingDemand=0,fastPredictedPeak=0;
  bool cooling=false,selfHeating=false,fastPath=false;
};
class AdaptiveThermalSupervisor {
 public:
  void reset(){*this=AdaptiveThermalSupervisor{};}
  void invalidate(){observer_.degrade();qualificationAnchor_=observer_.estimates().windows;d_.reason=Reason::ConfigChanged;ready_=false;}
  ThermalObserver &observer(){return observer_;}
  const ThermalObserver &observer()const{return observer_;}
  const Decision &decision()const{return d_;}
  // Diagnostic confidence is always the observer/model evidence. Self-heating
  // has its own confirmed state; never manufacture an 80% model-confidence
  // reading just because the protective channel is active.
  float effectiveConfidence()const{return observer_.estimates().confidence;}
  bool changedEnabled()const{return changedEnabled_;}
  void tick(uint32_t now,bool actualOn){
    observer_.tick(now,actualOn);
#if MAYAP_ADAPTIVE_FAST_PATH
    if(!fastTickSeen_){fastTickSeen_=true;fastTickAt_=now;return;}
    const uint32_t dt=since(now,fastTickAt_);fastTickAt_=now;
    if(dt>1000){fastEnergyMs_=0;return;}
    // A 60 s leaky energy window follows energy actually delivered by the SSR.
    // It cannot create heat demand and is reset by long scheduling stalls.
    fastEnergyMs_=std::max(0.0f,fastEnergyMs_*(1.0f-std::min(1.0f,dt/60000.0f)));
    if(actualOn)fastEnergyMs_+=dt;
#else
    (void)now;(void)actualOn;
#endif
  }
  void sample(uint32_t now,const Observation &o){observer_.sample(now,o);}
  Decision update(uint32_t now,bool enabled,float configured,const Observation &o,bool readOnly=false){
    changedEnabled_=enabled!=enabled_;const uint32_t dt=seen_?since(now,at_):0;seen_=true;at_=now;
    if(changedEnabled_){observer_.reset();ready_=false;enabled_=enabled;self_=confirm_=false;cool_=false;
      intervention_=false;resumeEffective_=configured;coolAt_=now;fastEnergyMs_=0;}
    if(!enabled){d_=Decision{};d_.effective=configured;intervention_=false;resumeEffective_=configured;return d_;}
    if(!std::isfinite(configured)||configured<0||configured>100||!o.sensor||
       !std::isfinite(o.pv)||!std::isfinite(o.raw)||o.safety||o.tune||o.test||o.maintenance||o.recovery){
      observer_.suspend(!o.sensor||o.safety||!std::isfinite(o.pv)||!std::isfinite(o.raw));
      d_=Decision{};d_.state=State::FaultBypass;d_.reason=Reason::SensorSafety;
      d_.effective=std::isfinite(configured)?bound(configured,0,100):0;ready_=false;
      qualificationAnchor_=observer_.estimates().windows;
      self_=confirm_=cool_=false;intervention_=false;resumeEffective_=d_.effective;coolAt_=now;return d_;
    }
    const auto &e=observer_.estimates();
    if(!ThermalObserver::finiteSeed(e.load,e.coast,e.coastSec,e.hold)||
       !std::isfinite(e.confidence)||!std::isfinite(e.rate)){
      observer_.reset();d_=Decision{};d_.effective=configured;d_.state=State::FaultBypass;
      d_.reason=Reason::InvalidWindow;self_=confirm_=cool_=ready_=intervention_=false;
      resumeEffective_=d_.effective;return d_;
    }
    if(readOnly){d_=Decision{};d_.state=State::Learning;d_.reason=Reason::Learning;
      d_.effective=configured;self_=confirm_=cool_=ready_=intervention_=false;
      resumeEffective_=configured;return d_;}
#if MAYAP_ADAPTIVE_FAST_PATH
    const float fastTarget=fastPathTarget(configured,o,e);
#else
    const float fastTarget=configured;
#endif
    const bool positive=e.learningValid&&o.fanStable&&!o.vent&&observer_.offMs(now)>=Policy::CoastMs&&
      o.pv>o.sp+Policy::SelfOnC&&e.rate>Policy::SelfRateCPerSec;
    if(positive&&!confirm_){confirm_=true;confirmAt_=now;}
    if(!positive)confirm_=false;
    if(confirm_&&since(now,confirmAt_)>=Policy::SelfConfirmMs)self_=true;
    if(self_&&o.pv<=o.sp+Policy::SelfOffC)self_=false;
    const bool wantCool=self_&&o.pv>o.sp+Policy::SelfOnC;
    if(wantCool!=cool_&&since(now,coolAt_)>=Policy::CoolingMinMs){cool_=wantCool;coolAt_=now;}
    if(self_||cool_){
      // A protective 0% cut must not destroy the previously qualified ceiling.
      // Save it once on entry so exit hands control back to the same bounded
      // authority instead of crawling from 0 at the normal +5 pct/min slew.
      if(!intervention_){resumeEffective_=bound(d_.effective,0,configured);intervention_=true;}
      d_.state=State::SelfHeating;d_.reason=Reason::SelfHeating;d_.selfHeating=self_;
      d_.cooling=cool_;d_.coolingDemand=cool_?100:0;d_.effective=0;ready_=true;return d_;
    }
    if(intervention_){
      d_.effective=bound(resumeEffective_,0,configured);
      intervention_=false;
      // Cooling/vent samples are invalid learning evidence. Keep the restored
      // pre-intervention ceiling as the starting point while fresh evidence
      // requalifies; ordinary degradation may then return it toward baseline.
      ready_=true;
    }
    if(!e.learningValid)qualificationAnchor_=e.windows;
    if(!e.learningValid || e.confidence<Policy::MediumConfidence || e.validMs<Policy::LearnMinMs || e.windows-qualificationAnchor_<Policy::MinQualifiedWindows){
      const float before=d_.effective;
      d_=Decision{};d_.state=e.windows?State::Degraded:State::Learning;
      d_.reason=e.learningValid?Reason::Learning:Reason::InvalidWindow;
#if MAYAP_ADAPTIVE_FAST_PATH
      if(!ready_){d_.effective=configured;ready_=true;}
      else d_.effective=slew(before,std::min(configured,fastTarget),dt);
#else
      d_.effective=ready_?std::min(configured,before+Policy::RisePctPerMin*std::min<uint32_t>(dt,1000)*.001f/60):configured;
#endif
      d_.fastPath=fastTarget<configured-0.01f;
      d_.fastPredictedPeak=fastPredictedPeak_;
      if(d_.fastPath){d_.state=State::Degraded;d_.reason=Reason::FastPath;}
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
    target=std::min(target,fastTarget);
    if(!ready_){d_.effective=configured;ready_=true;d_.state=State::Qualified;}
    const float delta=std::min<uint32_t>(dt,1000)*.001f/60;
    d_.effective=bound(target,d_.effective-Policy::FallPctPerMin*delta,d_.effective+Policy::RisePctPerMin*delta);
    d_.effective=bound(d_.effective,0,configured);d_.coolingDemand=0;d_.cooling=d_.selfHeating=false;
    d_.fastPath=fastTarget<configured-0.01f;d_.fastPredictedPeak=fastPredictedPeak_;
    d_.state=d_.effective<configured-0.01f?State::Adaptive:State::Qualified;
    d_.reason=d_.state==State::Adaptive?Reason::Limited:Reason::Qualified;return d_;
  }
 private:
  float slew(float current,float target,uint32_t dt)const{
    const float delta=std::min<uint32_t>(dt,1000)*.001f/60;
    return bound(target,current-Policy::FallPctPerMin*delta,current+Policy::RisePctPerMin*delta);
  }
#if MAYAP_ADAPTIVE_FAST_PATH
  float fastPathTarget(float configured,const Observation &o,const Estimates &e){
    fastPredictedPeak_=o.pv;
    if(!o.sensor||o.safety||o.vent||o.cooling||o.tune||o.test||o.maintenance||o.recovery||
       !std::isfinite(e.rate)||e.rate<=0.001f||fastEnergyMs_<3000)return configured;
    const float distance=o.sp-o.pv;
    // Convert delivered 16 kW on-time to a conservative residual-rise signal.
    // 240 kJ/C is deliberately below the qualified plant capacities, while
    // only 25% is credited as future coast to avoid suppressing slow plants.
    const float energyRise=fastEnergyMs_*Policy::BankWatts*.001f/240000.0f;
    const float trendRise=std::max(0.0f,e.rate)*45.0f;
    const float coastRise=std::max(trendRise,std::min(1.5f,energyRise*.25f+e.rate*20.0f));
    fastPredictedPeak_=o.pv+coastRise;
    const float approach=std::max(0.40f,std::min(2.50f,coastRise+0.40f));
    if(distance>=approach)return configured;
    const float remaining=bound((distance-coastRise+0.15f)/approach,0.0f,1.0f);
    // Early braking is shallow until the long-term observer has qualified the
    // plant as responsive. The result remains a ceiling and cannot add demand.
    // A qualified low-load index identifies a plant that converts delivered
    // energy quickly. Slow/heavy plants keep the shallow startup-only trim.
    const bool qualifiedResidual=e.learningValid&&e.confidence>=Policy::HighConfidence&&e.load<0.66f;
    const float minimumFraction=qualifiedResidual?0.20f:0.70f;
    return bound(configured*(minimumFraction+(1.0f-minimumFraction)*remaining),
                 Policy::MinAuthorityPct,configured);
  }
#endif
  ThermalObserver observer_;Decision d_{};uint32_t at_=0;
  uint32_t confirmAt_=0,coolAt_=0;bool self_=false,confirm_=false,cool_=false;
  uint32_t qualificationAnchor_=0;
  float resumeEffective_=100;
  float fastEnergyMs_=0,fastPredictedPeak_=0;uint32_t fastTickAt_=0;bool fastTickSeen_=false;
  bool enabled_=false,seen_=false,ready_=false,changedEnabled_=false,intervention_=false;
};

inline bool persistenceEligible(const Decision &d,const Estimates &e){
  const bool qualifiedState=d.state==State::Qualified||d.state==State::Adaptive;
  return qualifiedState&&e.learningValid&&std::isfinite(e.confidence)&&
    e.confidence>=Policy::HighConfidence&&e.validMs>=Policy::LearnMinMs&&
    e.windows>=Policy::MinQualifiedWindows;
}
}
