// Exercise production session parsing, retained serializer and portable timing.
#include <ArduinoJson.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>
#include "../MAYAP_INDUSTRIAL_v1_0_0/web_realtime_policy.h"
static uint32_t clockMs = 100, bootId = UINT32_MAX;
uint32_t millis() { return clockMs; }
bool timeReached(uint32_t now, uint32_t target) { return static_cast<int32_t>(now - target) >= 0; }
uint32_t elapsedMs(uint32_t now, uint32_t then) { return now - then; }
#include "actual-web-cadence-config.inc"
constexpr char MAYAP_FIRMWARE_VERSION[] = "1.0.0";
constexpr uint32_t WEB_SESSION_MAX_TTL_MS = 60000;
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
static int webMux;
struct WebClientLease { char id[40] = ""; uint32_t expiresAt = 0; };
static WebClientLease webClientLeases[8];
static bool webSessionActive = false, knownConfigValid = true;
static bool configDirty = false, eventSnapshotDirty = false, forceSnapshotPublish = false;
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
static MachineRuntime knownRuntime;
static bool knownRuntimeValid = false, failSnapshot = false;
static uint32_t webConfigRevision = 1;
struct Snapshot { uint32_t at; bool light; };
static std::vector<Snapshot> snapshots;
bool publishSnapshot(const MachineRuntime &rt, uint32_t revision) {
  assert(revision == webConfigRevision);
  if (failSnapshot) return false;
  snapshots.push_back({clockMs, rt.lightOn}); return true;
}
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
             bool config = false, bool log = false, uint32_t ttlMs = 15000) {
  JsonDocument doc;
  doc["clientId"] = id; doc["active"] = active; doc["ttlMs"] = ttlMs; doc["sync"] = sync;
  if (!legacy) doc["scope"] = "runtime";
  doc["config"] = config; doc["log"] = log;
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
  assert(webSessionActive && forceSnapshotPublish && !configDirty && !eventSnapshotDirty);
  assert(lastPublishedEventSequence == 7);
  session("browser-0001", true, true, false, true);
  assert(configDirty && !eventSnapshotDirty); configDirty = false;
  session("browser-0001", true, true, false, false, true);
  assert(eventSnapshotDirty && lastPublishedEventSequence == 0);
  configDirty = eventSnapshotDirty = false;
  session("browser-0001", true, true, true);
  assert(configDirty && eventSnapshotDirty); // Existing Web remains compatible.

  session("browser-0002", true);
  session("browser-0001", false); assert(webSessionActive);
  session("browser-0002", false); assert(!webSessionActive);
  session("browser-0002", true);
  clockMs += 15001; serviceSessionTimeout(clockMs); assert(!webSessionActive);
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
  for (uint32_t elapsed = 0; elapsed < 300000U; elapsed += 15000U) {
    clockMs = hiddenStart + elapsed;
    session("hidden-00001", true, false, false, false, false,
            300000U - elapsed < 45000U ? 300000U - elapsed : 45000U);
    session("visible-0001", true, false, false, false, false, 45000U);
    serviceSessionTimeout(clockMs);
    assert(webSessionActive);
  }
  clockMs = hiddenStart + 300000U;
  session("hidden-00001", false); serviceSessionTimeout(clockMs);
  assert(webSessionActive);
  session("visible-0001", false);
  assert(!webSessionActive);
  session("hidden-00001", true); assert(webSessionActive);
  // Fully suspended browser: TTL still expires without any Web timer.
  clockMs += 15000U; serviceSessionTimeout(clockMs);
  assert(!webSessionActive);
  // Controller completion/ACK can precede its 200 ms runtime mailbox update.
  // The forced sample still says OFF; the next real ON sample must not wait
  // for the usual one-second telemetry cadence before reaching the browser.
  clockMs = 1000; webSessionActive = true; forceSnapshotPublish = true;
  serviceSnapshotPublish(clockMs); assert(snapshots.empty());
  knownRuntimeValid = true; knownRuntime.lightOn = false;
  serviceSnapshotPublish(clockMs); assert(snapshots.size() == 1 && !snapshots.back().light);
  clockMs = 1050; forceSnapshotPublish = true; serviceSnapshotPublish(clockMs);
  assert(snapshots.size() == 2 && !snapshots.back().light && !forceSnapshotPublish);
  clockMs = 1200; knownRuntime.lightOn = true; serviceSnapshotPublish(clockMs);
  assert(snapshots.size() == 3 && snapshots.back().light && snapshots.back().at == 1200);
  // Temperature/PWM traffic keeps its original cadence; only lamp edges bypass it.
  for (uint32_t dt = 1; dt < WEB_SNAPSHOT_ACTIVE_INTERVAL_MS; ++dt) {
    clockMs = 1200 + dt; knownRuntime.temperature += .01f;
    knownRuntime.heaterOn = !knownRuntime.heaterOn; serviceSnapshotPublish(clockMs);
    assert(snapshots.size() == 3);
  }
  clockMs = 1200 + WEB_SNAPSHOT_ACTIVE_INTERVAL_MS; serviceSnapshotPublish(clockMs);
  assert(snapshots.size() == 4);
  // A full transport queue must not consume the edge; retry the actual state.
  clockMs += 20; knownRuntime.lightOn = false; failSnapshot = true;
  serviceSnapshotPublish(clockMs); assert(snapshots.size() == 4);
  failSnapshot = false; ++clockMs; serviceSnapshotPublish(clockMs);
  assert(snapshots.size() == 5 && !snapshots.back().light);
  // Edges work through millis wrap; a hidden browser still emits no live snapshots.
  clockMs = UINT32_MAX - 50; forceSnapshotPublish = true; serviceSnapshotPublish(clockMs);
  assert(snapshots.size() == 6);
  ++clockMs; knownRuntime.lightOn = true; serviceSnapshotPublish(clockMs);
  assert(snapshots.size() == 7 && snapshots.back().light);
  clockMs = 20; knownRuntime.lightOn = false; serviceSnapshotPublish(clockMs);
  assert(snapshots.size() == 8 && !snapshots.back().light);
  webSessionActive = false; ++clockMs; knownRuntime.lightOn = true; serviceSnapshotPublish(clockMs);
  assert(snapshots.size() == 8);
  webSessionActive = true; ++clockMs; serviceSnapshotPublish(clockMs);
  assert(snapshots.size() == 9 && snapshots.back().light);
  std::puts("Actual snapshot: ACK before runtime, immediate real lamp edges, unchanged temperature/PWM cadence, queue retry, hidden lease and rollover PASS");
  std::printf("Actual Web bootstrap/session: %zu-byte packet, retained, bounded cadence/retry, lazy sync, 8 leases, 300s warm renewals, independent visible browser, TTL and clock rollover OK\n", packetSize);
}
