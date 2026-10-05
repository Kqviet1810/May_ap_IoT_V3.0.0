#pragma once
#include <ArduinoJson.h>
// HTTP status is insufficient: only a matching durable receipt releases an event.
inline bool mayapDurableAlarmReceipt(const char *body,const char *eventId) {
  JsonDocument receipt;
  return !deserializeJson(receipt,body) && receipt["success"]==true &&
      receipt["durable"]==true && strcmp(receipt["event_id"] | "",eventId)==0;
}
