// Exercise production session parsing, retained serializer and portable timing.
#include <ArduinoJson.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <string>
#include "../MAYAP_INDUSTRIAL_v4_0_0/web_realtime_policy.h"
static uint32_t clockMs = 100, bootId = UINT32_MAX;
uint32_t millis() { return clockMs; }
bool timeReached(uint32_t now, uint32_t target) { return static_cast<int32_t>(now - target) >= 0; }
constexpr char MAYAP_FIRMWARE_VERSION[] = "4.0.0";
constexpr uint32_t WEB_SESSION_MAX_TTL_MS = 60000;
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
static int webMux;
struct WebClientLease { char id[40] = ""; uint32_t expiresAt = 0; };
static WebClientLease webClientLeases[8];
static bool webSessionActive = false, knownConfigValid = true, knownRemindersValid = true;
static bool configDirty = false, remindersDirty = false, eventSnapshotDirty = false, forceSnapshotPublish = false;
static struct { bool humidifierInstalled = false; } knownConfig;
static uint32_t lastSnapshotPublishAt = 99, lastPublishedEventSequence = 7;
struct Fault { uint16_t code = 112; uint8_t severity = 3; };
struct MachineRuntime {
  float temperature = 125.123f, humidity = 100.123f;
  char machineState[20] = "XXXXXXXXXXXXXXXXXXX";
  bool batchRunning = false, heaterOn = false, circulationFanOn = false,
    ventFanOn = false, humidifierOn = false, lightOn = false, sirenOn = false;
  uint32_t alarmMask = UINT32_MAX;
  uint16_t primaryFaultCode = UINT16_MAX;
  uint8_t activeFaultCount = 255, activeFaultDisplayCount = 1;
  Fault activeFaults[1];
};
static std::string wire, topic;
static bool retained = false;
const char *topicOf(const char *suffix) {
  static std::string value;
  value = std::string("mayap/v1/MAP-1234567890AB/") + suffix;
  return value.c_str();
}
bool publishJson(const char *suffix, const JsonDocument &doc, bool retain) {
  wire.clear(); serializeJson(doc, wire); topic = topicOf(suffix); retained = retain;
  return true;
}
#include "actual-web-connect.inc"
void session(const char *id, bool active, bool sync = false, bool legacy = false,
             bool config = false, bool reminders = false, bool log = false, uint32_t ttlMs = 15000) {
  JsonDocument doc;
  doc["clientId"] = id; doc["active"] = active; doc["ttlMs"] = ttlMs; doc["sync"] = sync;
  if (!legacy) doc["scope"] = "runtime";
  doc["config"] = config; doc["reminders"] = reminders; doc["log"] = log;
  handleSessionMessage(doc);
}
int main() {
  using namespace MayapWebRealtime;
  MachineRuntime rt;
  assert(publishBootstrap(rt, UINT32_MAX));
  assert(retained && topic == "mayap/v1/MAP-1234567890AB/bootstrap");
  assert(wire.size() + topic.size() + 7 <= BOOTSTRAP_PACKET_BUDGET);
  JsonDocument decoded; assert(!deserializeJson(decoded, wire));
  assert(decoded["bootId"] == UINT32_MAX && decoded["config"].isNull());
  assert(decoded["faultCode"] == UINT16_MAX && decoded["lightOn"] == false);
  const size_t packetSize = wire.size() + topic.size() + 7;
  knownConfig.humidifierInstalled = true; assert(publishBootstrap(rt, UINT32_MAX));
  assert(!deserializeJson(decoded, wire)); assert(decoded["humidifierInstalled"] == true);
  rt.temperature = NAN; rt.humidity = NAN;
  assert(publishBootstrap(rt, 1));
  assert(wire.find("\"temperature\":null") != std::string::npos);
  rt.temperature = 37.5; rt.humidity = 58;
  BootstrapCadence cadence;
  auto a = bootstrapState(rt, 7);
  assert(cadence.due(100, a)); cadence.attempted(100, a, true);
  assert(!cadence.due(20100, a)); assert(cadence.due(30100, a));
  rt.temperature += .01f; assert(!cadence.due(2200, bootstrapState(rt, 7)));
  rt.lightOn = true; auto b = bootstrapState(rt, 7);
  assert(!cadence.due(2099, b)); assert(cadence.due(2100, b));
  cadence.attempted(2100, b, false); assert(!cadence.due(2101, b));
  assert(cadence.due(4100, b)); cadence.attempted(4100, b, true);
  assert(!cadence.due(5000, b)); cadence.reset(); assert(cadence.due(5000, b));

  session("browser-0001", true, true);
  assert(webSessionActive && forceSnapshotPublish && !configDirty && !remindersDirty && !eventSnapshotDirty);
  assert(lastPublishedEventSequence == 7);
  session("browser-0001", true, true, false, true);
  assert(configDirty && !remindersDirty && !eventSnapshotDirty); configDirty = false;
  session("browser-0001", true, true, false, false, true, true);
  assert(remindersDirty && eventSnapshotDirty && lastPublishedEventSequence == 0);
  configDirty = remindersDirty = eventSnapshotDirty = false;
  session("browser-0001", true, true, true);
  assert(configDirty && remindersDirty && eventSnapshotDirty); // Existing Web remains compatible.

  PerformanceGrace grace;
  assert(grace.update(clockMs, webSessionActive));
  session("browser-0002", true);
  session("browser-0001", false); // Another foreground tab holds PERFORMANCE.
  assert(webSessionActive && grace.update(clockMs, webSessionActive));
  session("browser-0002", false);
  assert(!webSessionActive && grace.update(clockMs, webSessionActive));
  clockMs += 24999; assert(grace.update(clockMs, false));
  session("browser-0001", true); assert(grace.update(clockMs, true));
  session("browser-0001", false); assert(grace.update(clockMs, false));
  clockMs += 25000; assert(!grace.update(clockMs, false)); // Hidden long enough -> SAVE.
  session("browser-0002", true); assert(grace.update(clockMs, true)); // Return -> PERFORMANCE.
  clockMs += 15001; serviceSessionTimeout(clockMs);
  assert(!webSessionActive && grace.update(clockMs, false)); // Lost inactive packet: TTL + grace.
  clockMs += 25000; assert(!grace.update(clockMs, false));
  for (unsigned i = 0; i < 8; ++i) {
    char id[40]; snprintf(id, sizeof(id), "browser-%04u", i);
    session(id, true); assert(webSessionActive);
  }
  session("browser-9999", false); assert(webSessionActive); // Unknown ninth tab cannot clear leases.
  for (unsigned i = 0; i < 7; ++i) {
    char id[40]; snprintf(id, sizeof(id), "browser-%04u", i);
    session(id, false); assert(webSessionActive);
  }
  session("browser-0007", false); assert(!webSessionActive);
  // Web's actual warm policy: one hidden lease renews every 15s, bounded to
  // the five-minute deadline. A second visible browser remains independent.
  const uint32_t hiddenStart = UINT32_MAX - 100000U;
  clockMs = hiddenStart;
  PerformanceGrace warmGrace;
  for (uint32_t elapsed = 0; elapsed < 300000U; elapsed += 15000U) {
    clockMs = hiddenStart + elapsed;
    session("hidden-00001", true, false, false, false, false, false,
            300000U - elapsed < 45000U ? 300000U - elapsed : 45000U);
    session("visible-0001", true, false, false, false, false, false, 45000U);
    serviceSessionTimeout(clockMs);
    assert(webSessionActive && warmGrace.update(clockMs, webSessionActive));
  }
  clockMs = hiddenStart + 300000U;
  session("hidden-00001", false); serviceSessionTimeout(clockMs);
  assert(webSessionActive && warmGrace.update(clockMs, webSessionActive));
  session("visible-0001", false);
  assert(!webSessionActive && warmGrace.update(clockMs, false));
  clockMs += 24999U; assert(warmGrace.update(clockMs, false));
  clockMs += 1U; assert(!warmGrace.update(clockMs, false));
  session("hidden-00001", true); assert(warmGrace.update(clockMs, webSessionActive));
  // Fully suspended browser: TTL still expires without any Web timer.
  clockMs += 15000U; serviceSessionTimeout(clockMs);
  assert(!webSessionActive && warmGrace.update(clockMs, false));
  clockMs += 25000U; assert(!warmGrace.update(clockMs, false));
  PerformanceGrace rollover;
  assert(rollover.update(UINT32_MAX - 1000, false));
  assert(rollover.update(500, false)); assert(!rollover.update(25000, false));
  std::printf("Actual Web bootstrap/session: %zu-byte packet, retained, bounded cadence/retry, lazy sync, 8 leases, 300s warm renewals, independent visible browser, TTL, 25s grace and clock rollover OK\n", packetSize);
}
