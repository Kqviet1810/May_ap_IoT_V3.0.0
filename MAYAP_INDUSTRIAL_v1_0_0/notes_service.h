#pragma once
#include "notes_store.h"
#include "notes_retry_policy.h"
static_assert(EEPROM_ADDR_TEMP_HISTORY + TEMP_HISTORY_STORAGE_BYTES <= MayapNotes::BASE, "Notes overlap history");
static_assert(MayapNotes::END <= EEPROM_CAPACITY_BYTES, "Notes exceed AT24C512");
static_assert(MayapNotes::SLOT_BYTES % EEPROM_PAGE_SIZE == 0, "Notes bank alignment");
namespace MayapNotesOwner {
static Mayap::ExternalEeprom24xx eeprom;
static MayapNotes::Store<Mayap::ExternalEeprom24xx> store(eeprom);
static StaticQueue_t requestQueueBuffer, responseQueueBuffer;
static uint8_t requestBytes[sizeof(MayapNotes::Request)],responseBytes[sizeof(MayapNotes::Response)];
static QueueHandle_t requests=nullptr,responses=nullptr;
static StaticTask_t taskBuffer;
static StackType_t stack[8192/sizeof(StackType_t)];
static TaskHandle_t task=nullptr;
inline void captureWriteTrace(MayapNotes::Response &result) {
  if(result.ioStage!=MayapNotes::IoStage::Write)return;
  const auto trace=eeprom.lastWriteTrace();result.failedAddress=trace.address;result.writeReason=trace.reason;
  result.requested=trace.requested;result.written=trace.written;result.wireError=trace.error;
}
inline void run(void *) {
  // Only this owner touches the store. Fixed mailboxes; never touches actuators.
  static MayapNotes::Request request;
  static MayapNotes::Response response;
  bool ready=false,retry=false; uint32_t retryAt=0;
  MayapNotes::ReconcileBudget reconcileBudget;
  for(;;) {
    if(ready) {
      if(xQueueSend(responses,&response,0)==pdTRUE) {
        ready=false;
        if(response.ambiguous) {
          if(!reconcileBudget.active()) reconcileBudget.begin(millis());
          retry=!reconcileBudget.exhausted(millis());
          retryAt=millis();request.reconcile=retry;
        } else {
          retry=false;request.reconcile=false;reconcileBudget.reset();
        }
      }
    }
    else if(retry) {
      if(reconcileBudget.due(millis(),retryAt)) {
        reconcileBudget.markAttempt();
        response=store.process(request);captureWriteTrace(response);
        response.reconcileAttempts=reconcileBudget.attempts();
        // Only I/O uncertainty is retryable. Deterministic conflicts/corruption
        // must become a terminal result instead of consuming the whole budget.
        if(response.ambiguous&&response.code!=MayapNotes::Code::Eeprom) response.ambiguous=false;
        if(response.ambiguous&&reconcileBudget.exhausted(millis())) {
          response.ambiguous=false;response.retryExhausted=true;response.code=MayapNotes::Code::Eeprom;
        }
        ready=true;
      }
    }
    else if(xQueueReceive(requests,&request,0)==pdTRUE) {
      reconcileBudget.reset();request.reconcile=false;
      response=store.process(request);captureWriteTrace(response);ready=true;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
}
inline bool mayapNotesStart() {
  using namespace MayapNotesOwner;
  if(task)return true;
  requests=xQueueCreateStatic(1,sizeof(MayapNotes::Request),requestBytes,&requestQueueBuffer);
  responses=xQueueCreateStatic(1,sizeof(MayapNotes::Response),responseBytes,&responseQueueBuffer);
  if(!requests||!responses)return false;
  task=xTaskCreateStaticPinnedToCore(run,"mayap_notes",sizeof(stack),nullptr,1,stack,&taskBuffer,0);
  return task!=nullptr; // Optional feature failure must never restart local control.
}
inline bool mayapNotesSubmit(const MayapNotes::Request &request) {
  return MayapNotesOwner::task&&xQueueSend(MayapNotesOwner::requests,&request,0)==pdTRUE;
}
inline bool mayapNotesReceive(MayapNotes::Response &response) {
  return MayapNotesOwner::task&&xQueueReceive(MayapNotesOwner::responses,&response,0)==pdTRUE;
}
