#pragma once
#include "note_journal.h"
namespace MayapNoteMailbox {
// One operation, one result. I/O is owned by Arduino loop, network by WebSocket
// task. A completed result is retained until the transport has queued it.
static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
static MayapNoteJournal::Request request;
static MayapNoteJournal::Result result;
static MayapNoteJournal::Document document;
static ReminderSet reminderInbox{};
static uint32_t reminderRevision=0,reminderApplied=0;static bool reminderReady=false;
inline bool takeReminders(ReminderSet &r,uint32_t &revision){portENTER_CRITICAL(&mux);const bool available=reminderReady;
  if(available){r=reminderInbox;revision=reminderRevision;reminderReady=false;}portEXIT_CRITICAL(&mux);return available;}
inline uint32_t verifiedRevision(){portENTER_CRITICAL(&mux);const uint32_t r=reminderRevision;portEXIT_CRITICAL(&mux);return r;}
inline uint32_t appliedRevision(){portENTER_CRITICAL(&mux);const uint32_t r=reminderApplied;portEXIT_CRITICAL(&mux);return r;}
inline void applied(uint32_t revision){portENTER_CRITICAL(&mux);reminderApplied=revision;portEXIT_CRITICAL(&mux);}
enum class MountState:uint8_t {Mounting,Ready,Failed};
static MountState mountState=MountState::Mounting;
static MayapNoteJournal::Code mountCode=MayapNoteJournal::Code::Ok;
inline MountState status(MayapNoteJournal::Code &code){portENTER_CRITICAL(&mux);const auto value=mountState;code=mountCode;portEXIT_CRITICAL(&mux);return value;}
inline void mounted(MayapNoteJournal::Code code){portENTER_CRITICAL(&mux);mountCode=code;
  mountState=code==MayapNoteJournal::Code::Ok?MountState::Ready:MountState::Failed;portEXIT_CRITICAL(&mux);}
static bool busy=false,started=false,ready=false;
inline bool submit(const MayapNoteJournal::Request &r){
  portENTER_CRITICAL(&mux);const bool accepted=!busy;
  if(accepted){request=r;busy=true;started=ready=false;}portEXIT_CRITICAL(&mux);return accepted;
}
inline bool peek(MayapNoteJournal::Result &r,MayapNoteJournal::Document &n){
  portENTER_CRITICAL(&mux);const bool available=ready;
  if(available){r=result;if(r.hasNote)n=document;}portEXIT_CRITICAL(&mux);return available;
}
inline void consume(){portENTER_CRITICAL(&mux);busy=started=ready=false;portEXIT_CRITICAL(&mux);}
}
