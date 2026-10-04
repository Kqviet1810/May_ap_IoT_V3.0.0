#pragma once
#include "note_mailbox.h"
static_assert(EEPROM_ADDR_TEMP_HISTORY+TEMP_HISTORY_STORAGE_BYTES<=MayapNoteJournal::Base,"Journal overlaps history");
static_assert(MayapNoteJournal::End<=EEPROM_CAPACITY_BYTES,"Journal exceeds C512");
namespace MayapNoteStorage {
static Mayap::ExternalEeprom24xx io;
static MayapNoteJournal::Journal<Mayap::ExternalEeprom24xx> journal(io);
static bool bootStarted=false,bootFinished=false;
inline void update(){
  using namespace MayapNoteMailbox;
  if(!bootStarted){
    portENTER_CRITICAL(&mux);if(!busy){request=MayapNoteJournal::Request{};request.operation=MayapNoteJournal::Operation::ReadReminders;
      snprintf(request.note.id,sizeof(request.note.id),"%s",MayapNoteJournal::ReminderId);busy=true;started=ready=false;bootStarted=true;}portEXIT_CRITICAL(&mux);
  }
  portENTER_CRITICAL(&mux);const bool run=busy&&!ready,begin=run&&!started;
  if(begin)started=true;
  portEXIT_CRITICAL(&mux);
  if(!run)return;
  // request cannot change while busy; no large copy or I2C under a spinlock.
  if(begin)journal.begin(request);
  journal.step();
  if(!journal.ready())return;
  const bool reminder=request.operation==MayapNoteJournal::Operation::ReadReminders||request.operation==MayapNoteJournal::Operation::SaveReminders;
  if(reminder&&journal.result().code==MayapNoteJournal::Code::Ok){
    portENTER_CRITICAL(&mux);reminderInbox=ReminderSet{};
    if(journal.result().hasNote){static_assert(sizeof(reminderInbox)==sizeof(MayapNoteJournal::ReminderPayload),"Reminder layout");
      memcpy(&reminderInbox,journal.document().content,sizeof(reminderInbox));}
    reminderRevision=journal.result().generation;reminderReady=true;portEXIT_CRITICAL(&mux);
  }
  portENTER_CRITICAL(&mux);result=journal.result();if(result.hasNote)document=journal.document();ready=true;portEXIT_CRITICAL(&mux);
  if(bootStarted&&!bootFinished){mounted(result.code);bootFinished=true;consume();}
  else if(result.code==MayapNoteJournal::Code::Io||result.code==MayapNoteJournal::Code::Corrupt||result.code==MayapNoteJournal::Code::Ok)mounted(result.code);
  if(result.code==MayapNoteJournal::Code::Io){const auto trace=io.lastWriteTrace();const auto read=io.lastReadTrace();
    mayapSerialPrintf(false,"[NOTE-JOURNAL] io addr=0x%04X reason=%u requested=%u written=%u wire=%u\n",trace.address,trace.reason,trace.requested,trace.written,trace.error);
    mayapSerialPrintf(false,"[NOTE-JOURNAL] read addr=0x%04X reason=%u requested=%u got=%u wire=%u\n",read.address,read.reason,read.requested,read.written,read.error);}
  mayapSerialPrintf(false,"[NOTE-JOURNAL] code=%s seq=%lu addr=0x%04X\n",MayapNoteJournal::codeText(result.code),static_cast<unsigned long>(result.generation),result.address);
}
}
