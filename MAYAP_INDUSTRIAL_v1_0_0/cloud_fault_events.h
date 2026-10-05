#pragma once
// Bounded control -> Cloud mailbox. No heap, socket, wait, or network I/O.
#define MAYAP_CLOUD_FAULT_EVENTS 1
namespace MayapCloudFaultEvents {
constexpr uint8_t CAPACITY = 64U, STATE_CAPACITY = 48U;
struct Event { uint32_t at; uint16_t code; uint8_t severity; bool active;
  Event(uint32_t t=0,uint16_t c=0,uint8_t s=0,bool a=false):at(t),code(c),severity(s),active(a){} };
struct State { Event event{}; bool used=false, resync=false; };
static Event events[CAPACITY];
static State states[STATE_CAPACITY];
static uint8_t head=0,count=0;
static uint32_t overflow=0;
static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
inline void record(uint16_t code,uint8_t severity,bool active,uint32_t at) {
  portENTER_CRITICAL(&mux);
  State *state=nullptr;
  for(auto &s:states) if(s.used && s.event.code==code){state=&s;break;}
  if(!state) for(auto &s:states) if(!s.used){state=&s;break;}
  if(state && (!state->used || state->event.active!=active)) {
    state->used=true;state->event={at,code,severity,active};
    if(count<CAPACITY) {events[(head+count)%CAPACITY]=state->event;++count;}
    else {__atomic_fetch_add(&overflow,1U,__ATOMIC_RELAXED);state->resync=true;}
  }
  portEXIT_CRITICAL(&mux);
}
inline bool peek(Event &event) {
  portENTER_CRITICAL(&mux);
  bool found=count!=0;
  uint8_t selected=0U;
  // Only Cloud scans priority. A critical edge cannot pass its own earlier
  // transition; other warning edges cannot hide it behind queue backpressure.
  for(uint8_t n=0U;n<count;++n) {
    const Event &candidate=events[(head+n)%CAPACITY];
    if(candidate.severity<2U) continue;
    bool predecessor=false;
    for(uint8_t k=0;k<n;++k) if(events[(head+k)%CAPACITY].code==candidate.code){predecessor=true;break;}
    if(!predecessor){selected=n;break;}
  }
  if(found) event=events[(head+selected)%CAPACITY];
  else for(auto &s:states) if(s.resync){event=s.event;found=true;break;}
  portEXIT_CRITICAL(&mux);
  return found;
}
inline void consume(const Event &event) {
  portENTER_CRITICAL(&mux);
  bool removed=false;
  for(uint8_t n=0;n<count;++n) {
    const Event &queued=events[(head+n)%CAPACITY];
    if(queued.code!=event.code || queued.at!=event.at || queued.active!=event.active) continue;
    for(uint8_t k=n;k+1U<count;++k) events[(head+k)%CAPACITY]=events[(head+k+1U)%CAPACITY];
    --count;removed=true;break;
  }
  if(!removed) for(auto &s:states) if(s.resync && s.event.code==event.code && s.event.at==event.at && s.event.active==event.active){s.resync=false;break;}
  portEXIT_CRITICAL(&mux);
}
}
inline void mayapCloudRecordFault(uint16_t code,uint8_t severity,bool active,uint32_t now) {
  MayapCloudFaultEvents::record(code,severity,active,now);
}
