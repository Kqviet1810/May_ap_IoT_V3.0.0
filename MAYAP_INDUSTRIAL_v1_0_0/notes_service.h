#pragma once
#include "notes_store.h"
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
  for(;;) {
    if(ready) { if(xQueueSend(responses,&response,0)==pdTRUE){ready=false;retry=response.ambiguous;retryAt=millis();request.reconcile=retry;} }
    else if(retry) { if(static_cast<uint32_t>(millis()-retryAt)>=2000U){response=store.process(request);captureWriteTrace(response);ready=true;} }
    else if(xQueueReceive(requests,&request,0)==pdTRUE){response=store.process(request);captureWriteTrace(response);ready=true;}
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
