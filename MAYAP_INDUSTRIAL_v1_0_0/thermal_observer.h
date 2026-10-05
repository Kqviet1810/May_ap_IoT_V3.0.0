#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// One probe: these are apparent whole-system responses, never spatial sensors.
namespace MayapAdaptive {
#ifndef MAYAP_ADAPTIVE_FAST_PATH
#define MAYAP_ADAPTIVE_FAST_PATH 1
#endif
namespace Policy {
constexpr uint32_t StartupMs=180000, WindowMs=120000, CoastMs=180000;
constexpr uint32_t SelfConfirmMs=60000, CoolingMinMs=120000, LearnMinMs=600000;
constexpr uint32_t DiagnosticMs=30000, SaveMinMs=3600000, ModelMaxAgeSec=604800;
constexpr float BankWatts=16000, MinEnergyJ=160000, HoldBandC=0.15f;
constexpr float HoldRateCPerSec=0.001f, SelfRateCPerSec=0.0003f;
constexpr float JumpC=0.35f, HighMarginC=0.3f;
constexpr float MediumConfidence=50, HighConfidence=80;
constexpr float RisePctPerMin=5, FallPctPerMin=20, MinAuthorityPct=10;
constexpr float ApproachMinC=0.15f, ApproachMaxC=1.0f;
constexpr float SelfOnC=0.12f, SelfOffC=0.04f, ConfidenceLoss=0.5f;
constexpr float LoadConfidenceGain=8, CoastConfidenceGain=12, HoldConfidenceGain=12;
constexpr uint32_t MinQualifiedWindows=5;
constexpr uint8_t RateSamples=64; // 126s at production 2s poll; preserves slow X10 staircases
constexpr float DoorRawJumpC=0.50f;
}
inline uint32_t since(uint32_t now,uint32_t then){return now-then;}
inline float bound(float x,float lo,float hi){return std::max(lo,std::min(hi,x));}
struct Observation {
  float pv=0,raw=0,sp=0,high=0,requested=0,effective=0;
  bool sensor=false,fanStable=false,vent=false,cooling=false,tune=false;
  bool safety=false,recovery=false,test=false,maintenance=false;
  bool valid()const{return sensor && std::isfinite(pv) && std::isfinite(raw) &&
    std::isfinite(sp) && std::isfinite(high) && fanStable && !vent && !cooling &&
    !tune && !safety && !recovery && !test && !maintenance &&
    std::max(pv,raw)<high-Policy::HighMarginC;}
};
struct Estimates {
  float load=0.5f,coast=0,coastSec=0,hold=0,confidence=0,rate=0;
  uint32_t windows=0,loadWindows=0,coastWindows=0,holdWindows=0;
  uint32_t validMs=0;bool learningValid=false;
};
class ThermalObserver {
 public:
  void reset(){*this=ThermalObserver{};}
  void degrade(){e_.confidence*=Policy::ConfidenceLoss;e_.validMs=0;clearWindow();guarded_=false;}
  void suspend(bool critical=false){
    if(critical)e_.confidence=0;
    else if(!invalid_)e_.confidence*=Policy::ConfidenceLoss;
    invalid_=true;e_.learningValid=false;e_.validMs=0;clearWindow();
  }
  void seed(float load,float coast,float coastSec,float hold){
    if(!finiteSeed(load,coast,coastSec,hold))return;
    e_.load=load;e_.coast=coast;e_.coastSec=coastSec;e_.hold=hold;
    e_.confidence=10; // seed is not evidence: no qualified windows restored.
  }
  static bool finiteSeed(float l,float c,float t,float h){return
    std::isfinite(l)&&l>=0&&l<=1&&std::isfinite(c)&&c>=0&&c<=5&&
    std::isfinite(t)&&t>=0&&t<=Policy::CoastMs*.001f&&std::isfinite(h)&&h>=0&&h<=100;}
  // Integrate the preceding REAL GPIO state, not requested duty. O(1) per tick.
  void tick(uint32_t now,bool actualOn){
    if(!tickSeen_){tickSeen_=true;tickAt_=now;return;}
    const uint32_t dt=since(now,tickAt_);tickAt_=now;
    if(dt>1000){degrade();offSeen_=false;return;}
    if(actualOn){onMs_+=dt;episodeOnMs_+=dt;offSeen_=false;coastActive_=false;}
    else if(!offSeen_){offSeen_=true;offAt_=now;offTemp_=lastPv_;peak_=lastPv_;peakAt_=now;}
  }
  void sample(uint32_t now,const Observation &o){
    const bool finite=std::isfinite(o.pv);const uint32_t dt=sampleSeen_?since(now,sampleAt_):0;
    const bool jump=sampleSeen_&&finite&&std::fabs(o.pv-lastPv_)>Policy::JumpC;
#if MAYAP_ADAPTIVE_FAST_PATH
    const bool setpointChange=sampleSeen_&&std::isfinite(o.sp)&&std::fabs(o.sp-lastSp_)>0.01f;
    const bool doorDisturbance=sampleSeen_&&std::isfinite(o.raw)&&std::isfinite(lastRaw_)&&
      std::fabs(o.raw-lastRaw_)>Policy::DoorRawJumpC;
#else
    const bool setpointChange=false,doorDisturbance=false;
#endif
    sampleAt_=now;sampleSeen_=true;
    if(!finite){e_.confidence=0;e_.learningValid=false;clearWindow();rates_=0;return;}
    lastPv_=o.pv;lastRaw_=o.raw;lastSp_=o.sp;
    if(jump||setpointChange||doorDisturbance||dt>10000){degrade();rates_=0;}
    updateRate(now,o.pv);
    if(!o.valid()||jump||setpointChange||doorDisturbance||dt>10000){
      e_.learningValid=false;clearWindow();guarded_=true;guardAt_=now;
      if(!o.sensor||o.safety)e_.confidence=0;
      else if(!invalid_)e_.confidence*=Policy::ConfidenceLoss;
      e_.validMs=0;
      invalid_=true;return;
    }
    if(invalid_){guardAt_=now;guarded_=true;invalid_=false;}
    if(!guarded_){guardAt_=now;guarded_=true;}
    if(since(now,guardAt_)<Policy::StartupMs){e_.learningValid=false;return;}
    e_.learningValid=true;e_.validMs+=dt;
    if(offSeen_ && episodeOnMs_>=Policy::MinEnergyJ/Policy::BankWatts*1000){
      coastActive_=true;
      if(o.pv>peak_){peak_=o.pv;peakAt_=now;}
      if(since(now,offAt_)>=Policy::CoastMs){
        const float rise=std::max(0.0f,peak_-offTemp_);
        if(rise<=5 && (e_.coastWindows<2||rise<=std::max(0.3f,e_.coast*3))){
          coastHistory_[coastHead_]=rise;coastHead_=(coastHead_+1)%5;
          coastCount_=std::min<uint8_t>(5,coastCount_+1);
          float values[5];const uint8_t count=coastCount_;
          for(uint8_t i=0;i<count;++i)values[i]=coastHistory_[i];
          for(uint8_t i=0;i<count;++i)for(uint8_t j=i+1;j<count;++j)
            if(values[j]<values[i])std::swap(values[j],values[i]);
          e_.coast=values[count/2];
          // The evidence window is 180 s. A later peak cannot justify a
          // longer model horizon; keep learned seeds inside finiteSeed().
          const float coastSec=std::min(Policy::CoastMs*.001f,
              since(peakAt_,offAt_)*.001f);
          e_.coastSec=e_.coastWindows?e_.coastSec*.75f+coastSec*.25f:coastSec;
          ++e_.coastWindows;qualify(Policy::CoastConfidenceGain);
        }
        episodeOnMs_=0;coastActive_=false;
      }
    }
    if(!windowSeen_){windowSeen_=true;windowAt_=now;windowPv_=o.pv;windowOn_=onMs_;holdEligible_=true;}
    holdEligible_=holdEligible_&&std::fabs(o.pv-o.sp)<=Policy::HoldBandC&&std::fabs(e_.rate)<=Policy::HoldRateCPerSec;
    const uint32_t elapsed=since(now,windowAt_);
    if(elapsed<Policy::WindowMs)return;
    const double energy=(onMs_-windowOn_)*Policy::BankWatts*.001;
    const float delta=o.pv-windowPv_;
    if(energy>=Policy::MinEnergyJ&&delta>=0.1f&&o.pv<o.sp-Policy::HoldBandC){
      const float response=static_cast<float>(delta/(energy/1000000));
      const float load=1/(1+response); // normalized inverse C/MJ response.
      if(e_.loadWindows && std::fabs(load-e_.load)>0.3f)e_.confidence*=Policy::ConfidenceLoss;
      e_.load=e_.loadWindows?e_.load*.75f+load*.25f:load;++e_.loadWindows;qualify(Policy::LoadConfidenceGain);
    }
    if(holdEligible_){
      const float hold=100.0f*(onMs_-windowOn_)/elapsed;
      if(e_.holdWindows && std::fabs(hold-e_.hold)>10)e_.confidence*=Policy::ConfidenceLoss;
      e_.hold=e_.holdWindows?e_.hold*.75f+hold*.25f:hold;++e_.holdWindows;qualify(Policy::HoldConfidenceGain);
    }
    windowSeen_=false;
  }
  const Estimates &estimates()const{return e_;}
  uint64_t actualOnMs()const{return onMs_;}
  uint32_t offMs(uint32_t now)const{return offSeen_?since(now,offAt_):0;}
 private:
  void clearWindow(){windowSeen_=false;coastActive_=false;episodeOnMs_=0;}
  void qualify(float amount){++e_.windows;e_.confidence=std::min(100.0f,e_.confidence+amount);}
  void updateRate(uint32_t now,float pv){
    times_[rateHead_]=now;temperatures_[rateHead_]=pv;rateHead_=(rateHead_+1)%Policy::RateSamples;rates_=std::min<uint8_t>(Policy::RateSamples,rates_+1);
    if(rates_<8){e_.rate=0;return;}
    const uint8_t first=(rateHead_+Policy::RateSamples-rates_)%Policy::RateSamples;double sx=0,sy=0,sxx=0,sxy=0;
    for(uint8_t i=0;i<rates_;++i){const uint8_t j=(first+i)%Policy::RateSamples;const double x=since(times_[j],times_[first])*.001,y=temperatures_[j];sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;}
    const double denominator=rates_*sxx-sx*sx;
    const float slope=denominator>0?static_cast<float>((rates_*sxy-sx*sy)/denominator):0;
    e_.rate=e_.rate*.5f+slope*.5f;
  }
  Estimates e_{};uint64_t onMs_=0,windowOn_=0,episodeOnMs_=0;
  uint32_t tickAt_=0,sampleAt_=0,guardAt_=0,windowAt_=0,offAt_=0,peakAt_=0;
  float lastPv_=0,lastRaw_=NAN,lastSp_=NAN,windowPv_=0,offTemp_=0,peak_=0,coastHistory_[5]{};
  uint8_t coastCount_=0,coastHead_=0;uint32_t times_[Policy::RateSamples]{};float temperatures_[Policy::RateSamples]{};
  uint8_t rates_=0,rateHead_=0;
  bool tickSeen_=false,sampleSeen_=false,guarded_=false,invalid_=false;
  bool windowSeen_=false,offSeen_=false,coastActive_=false,holdEligible_=false;
};
}
