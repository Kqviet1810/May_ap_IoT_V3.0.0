#pragma once
#include "adaptive_model.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>

// Sole flash owner is Arduino loop(), never the control task. Mailbox copies
// are fixed-size and the critical section contains no storage/network calls.
namespace MayapAdaptive {
class ModelStorage {
 public:
  void offer(const Model &model){portENTER_CRITICAL(&mux_);pending_=model;hasPending_=true;portEXIT_CRITICAL(&mux_);}
  void discardPending(){portENTER_CRITICAL(&mux_);hasPending_=false;portEXIT_CRITICAL(&mux_);}
  bool takeInvalid(){portENTER_CRITICAL(&mux_);const bool invalid=invalidRecord_;invalidRecord_=false;portEXIT_CRITICAL(&mux_);return invalid;}
  bool takeSeed(Model &model){
    portENTER_CRITICAL(&mux_);const bool ready=seedReady_;
    if(ready){model=seed_;seedReady_=false;}portEXIT_CRITICAL(&mux_);return ready;
  }
  void service(uint32_t now){
    if(!initialized_){
      initialized_=true;available_=prefs_.begin("mayap_thermal",false);
      if(!available_)return;
      Model a{},b{};const bool va=prefs_.getBytes("a",&a,sizeof(a))==sizeof(a)&&validModel(a);
      const bool vb=prefs_.getBytes("b",&b,sizeof(b))==sizeof(b)&&validModel(b);
      if((prefs_.isKey("a")&&!va)||(prefs_.isKey("b")&&!vb)){
        portENTER_CRITICAL(&mux_);invalidRecord_=true;portEXIT_CRITICAL(&mux_);
      }
      if(va||vb){
        const bool useA=!vb||(va&&static_cast<int32_t>(a.sequence-b.sequence)>=0);
        activeA_=useA;sequence_=useA?a.sequence:b.sequence;
        portENTER_CRITICAL(&mux_);seed_=useA?a:b;seedReady_=true;portEXIT_CRITICAL(&mux_);
      }
      lastSave_=now;return;
    }
    if(!available_||since(now,lastSave_)<Policy::SaveMinMs)return;
    Model model{};portENTER_CRITICAL(&mux_);const bool pending=hasPending_;
    if(pending){model=pending_;hasPending_=false;}portEXIT_CRITICAL(&mux_);
    if(!pending||!validModel(model)||model.epoch==0)return;
    lastSave_=now; // failed writes are wear/backoff bounded as well.
    model.sequence=sequence_+1;model.crc=modelCrc(model);
    const char *key=activeA_?"b":"a";Model verify{};
    if(prefs_.putBytes(key,&model,sizeof(model))==sizeof(model)&&
       prefs_.getBytes(key,&verify,sizeof(verify))==sizeof(verify)&&validModel(verify)&&
       std::memcmp(&model,&verify,sizeof(model))==0){activeA_=!activeA_;sequence_=model.sequence;}
  }
 private:
  Preferences prefs_;portMUX_TYPE mux_=portMUX_INITIALIZER_UNLOCKED;
  Model pending_{},seed_{};bool hasPending_=false,seedReady_=false,invalidRecord_=false;
  bool initialized_=false,available_=false,activeA_=false;uint32_t sequence_=0,lastSave_=0;
};
static ModelStorage modelStorage;
}
