// Compile actual production helpers against bounded/failing transports.
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <algorithm>
#include <string>
#include <vector>
#define MAYAP_DIAGNOSTIC_SERIAL 1
#define SERIAL_DEBUG_DEFAULT_ON true
#define portMUX_INITIALIZER_UNLOCKED 0
using portMUX_TYPE = int;
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
static uint32_t clockMs=100;
uint32_t millis() { return clockMs; }
struct FakeSerial {
  int room=64; size_t maxWrite=64; std::string output;
  int availableForWrite() { return room; }
  size_t write(const uint8_t *p, size_t n) {
    const size_t sent=std::min(maxWrite,n); output.append(reinterpret_cast<const char *>(p),sent); return sent;
  }
} Serial;
#include "actual-serial_diagnostics.inc"
struct FakeEsp { uint32_t free=85000, largest=31732;
  uint32_t getFreeHeap() { return free; } uint32_t getMaxAllocHeap() { return largest; }
} ESP;
#include "actual-network_io_guard.inc"
class Stream {
 public:
  virtual ~Stream() {}
  virtual size_t write(uint8_t)=0;
  virtual size_t write(const uint8_t *,size_t)=0;
  virtual int available()=0; virtual int read()=0; virtual int peek()=0; virtual void flush()=0;
};
struct HTTPClient {
  int size=-1, result=0; unsigned calls=0; std::vector<std::string> chunks;
  int getSize() { return size; }
  int writeToStream(Stream *s) {
    ++calls; int written=0;
    for (const auto &p:chunks) {
      const size_t n=s->write(reinterpret_cast<const uint8_t *>(p.data()),p.size());
      if (n!=p.size()) return -10;
      written+=n;
    }
    return result<0 ? result : written;
  }
};
#include "actual-bounded_http.inc"
constexpr uint8_t CLOUD_OUTBOX_SIZE=8;
namespace MayapCloudInternal {
enum class NotifyLevel : uint8_t { Info, Warning, Critical, System };
#include "actual-cloud-outbox.inc"
enum class ConnectivityMode { Offline, Online };
struct NetworkStatus { ConnectivityMode requestedMode=ConnectivityMode::Online; bool connected=true; } network;
static uint8_t pinResetRequestFlag=0;
static unsigned resetRequests=0;
static bool resetDeferred=false, resetSuccess=true;
NetworkStatus mayapGetNetworkStatus() { return network; }
bool sendResetPin() {
  requestDeferred=resetDeferred;
  if (resetDeferred) return false;
  ++resetRequests; return resetSuccess;
}
#include "actual-cloud-pin-reset.inc"
}
struct HmiEventItem { uint32_t sequence=0; };
struct HmiEventSnapshot { uint32_t sourceSequence=0; uint8_t count=0; HmiEventItem items[16]; };
static HmiEventSnapshot pendingEventSnapshot;
static portMUX_TYPE webMux=0;
static bool eventSnapshotDirty=false;
static uint32_t lastPublishedEventSequence=0, failSequence=0;
static std::vector<uint32_t> published;
bool publishLogEntry(const HmiEventItem &item) {
  if (item.sequence==failSequence) return false;
  published.push_back(item.sequence); return true;
}
#include "actual-event-publish.inc"
constexpr int ESP_OK=0, ESP_FAIL=-1;
enum wifi_ps_type_t { WIFI_PS_NONE, WIFI_PS_MIN_MODEM };
static wifi_ps_type_t radioPs=WIFI_PS_MIN_MODEM;
static bool radioReady=true, highPerfWifiApplied=false, wifiPowerModeValid=false;
static bool webSessionActive=true;
#include "../MAYAP_INDUSTRIAL_v4_0_0/web_realtime_policy.h"
using MayapCloudInternal::mayapGetNetworkStatus;
static MayapWebRealtime::PerformanceGrace wifiPerformanceGrace;
static unsigned psWrites=0;
int esp_wifi_get_ps(wifi_ps_type_t *ps) { if (!radioReady) return ESP_FAIL; *ps=radioPs; return ESP_OK; }
int esp_wifi_set_ps(wifi_ps_type_t ps) { ++psWrites; if (!radioReady) return ESP_FAIL; radioPs=ps; return ESP_OK; }
#include "actual-wifi-power.inc"
int main() {
  { MayapTlsOperation cloud(MayapTlsKind::Cloud); assert(cloud && mayapTlsBusy());
    MayapTlsOperation other; assert(!other);
    MayapNetworkBatchOperation bulk; assert(!bulk); }
  assert(!mayapTlsBusy());
  ESP.free=73727;
  { MayapTlsOperation cloud(MayapTlsKind::Cloud); assert(!cloud && !mayapTlsBusy()); }
  ESP.free=73728;
  { MayapTlsOperation cloud(MayapTlsKind::Cloud); assert(cloud); }
  ESP.free=85000; ESP.largest=24575;
  { MayapTlsOperation cloud(MayapTlsKind::Cloud); assert(!cloud); }
  ESP.largest=24576;
  { MayapNetworkBatchOperation bulk; assert(bulk);
    MayapTlsOperation cloud(MayapTlsKind::Cloud); assert(!cloud); }
  ESP.free=24575;
  { MayapNetworkBatchOperation bulk; assert(!bulk); }
  ESP.free=49151;
  { MayapTlsOperation mqtt; assert(!mqtt); }
  ESP.free=49152;
  { MayapTlsOperation mqtt; assert(mqtt); }
  assert(!mayapTlsBusy() && mayapTlsDeferredCount()>=4);

  char bounded[10]; HTTPClient http;
  http.size=20; assert(!mayapReadBoundedHttpBody(http,bounded,sizeof(bounded)) && http.calls==0);
  http.size=-1; http.chunks={"abc","defghi"};
  assert(mayapReadBoundedHttpBody(http,bounded,sizeof(bounded)) && !strcmp(bounded,"abcdefghi"));
  http.chunks={"abc","defghij"};
  assert(!mayapReadBoundedHttpBody(http,bounded,sizeof(bounded)) && !strcmp(bounded,"abc"));
  http.size=8; http.chunks={"abc"};
  assert(!mayapReadBoundedHttpBody(http,bounded,sizeof(bounded))); // truncated Content-Length
  http.size=-1; http.result=-11;
  assert(!mayapReadBoundedHttpBody(http,bounded,sizeof(bounded)));

  Serial.room=0; mayapSerialPrintf(false,"line one\n");
  mayapSerialDrain(); assert(Serial.output.empty());
  Serial.room=7; Serial.maxWrite=3;
  for(unsigned i=0;i<40;++i) mayapSerialDrain();
  assert(Serial.output=="[t=100] line one\n");
  Serial.room=0;
  for(unsigned i=0;i<8;++i) mayapSerialPrintf(false,"noise %u\n",i);
  mayapSerialPrintf(false,"[FAULT] SET 501\n");
  assert(MayapSerialInternal::dropped==1 && MayapSerialInternal::criticalDropped==0);
  Serial.room=64; Serial.maxWrite=64;
  for(unsigned i=0;i<30;++i) mayapSerialDrain();
  assert(Serial.output.find("[FAULT] SET 501")!=std::string::npos);
  assert(Serial.output.find("[SERIAL] dropped=1 critical=0")!=std::string::npos);
  mayapSerialPrintf(false,"discard on EXIT\n"); mayapSetSerialDebugEnabled(false);
  mayapSerialPrintf(true,"[SERIAL] OFF (EXIT)\n");
  mayapSerialPrintf(false,"must not leak\n");
  for(unsigned i=0;i<10;++i) mayapSerialDrain();
  assert(Serial.output.find("discard on EXIT")==std::string::npos);
  assert(Serial.output.find("must not leak")==std::string::npos);
  assert(Serial.output.find("[SERIAL] OFF (EXIT)")!=std::string::npos);
  Serial.room=0;
  for(unsigned i=0;i<8;++i) mayapSerialPrintf(true,"toggle %u\n",i);
  mayapSerialPrintf(true,"final OFF\n"); Serial.room=64;
  for(unsigned i=0;i<20;++i) mayapSerialDrain();
  assert(Serial.output.find("final OFF")!=std::string::npos);

  using namespace MayapCloudInternal;
  (void)lastSendAt; (void)lastRequestFinishedAt; (void)requestDeferred;
  pinResetRequestFlag=1; resetDeferred=true;
  servicePinReset(); servicePinReset();
  assert(pinResetRequestFlag==1 && resetRequests==0); // Not sent while busy.
  resetDeferred=false; servicePinReset(); servicePinReset();
  assert(pinResetRequestFlag==0 && resetRequests==1);
  pinResetRequestFlag=1; resetSuccess=false; servicePinReset(); servicePinReset();
  assert(pinResetRequestFlag==0 && resetRequests==2); // No ambiguous replay.
  pinResetRequestFlag=1; network.connected=false; servicePinReset();
  assert(pinResetRequestFlag==0 && resetRequests==2);
  assert(enqueueRaw("FAULT_501",NotifyLevel::Warning,false,"first",false,0,0));
  assert(enqueueRaw("FAULT_501",NotifyLevel::Warning,false,"repeat",false,0,0));
  assert(outboxCount==1 && !strcmp(outbox[outboxHead].message,"repeat"));
  assert(enqueueRaw("FAULT_501",NotifyLevel::Warning,true,"clear",false,0,0));
  assert(enqueueRaw("FAULT_501",NotifyLevel::Warning,false,"again",false,0,0));
  assert(outboxCount==3 && !outbox[0].resolved && outbox[1].resolved && !outbox[2].resolved);
  outboxHead=outboxTail=outboxCount=0;
  for(unsigned i=0;i<8;++i) {
    char key[24]; std::snprintf(key,sizeof(key),"CRITICAL_%u",i);
    assert(enqueueRaw(key,NotifyLevel::Critical,false,"critical",false,0,0));
  }
  assert(!enqueueRaw("ROUTINE",NotifyLevel::Info,false,"routine",false,0,0));
  assert(outboxCount==8 && outboxCriticalDropped==0);
  assert(enqueueRaw("CRITICAL_NEW",NotifyLevel::Critical,false,"critical",false,0,0));
  assert(outboxCount==8 && outboxCriticalDropped==1);

  pendingEventSnapshot.sourceSequence=12; pendingEventSnapshot.count=12;
  for(unsigned i=0;i<12;++i) pendingEventSnapshot.items[i].sequence=12-i;
  eventSnapshotDirty=true; failSequence=3;
  serviceEventLogPublish(); assert(lastPublishedEventSequence==2 && eventSnapshotDirty);
  failSequence=0; serviceEventLogPublish(); assert(lastPublishedEventSequence==7 && eventSnapshotDirty);
  serviceEventLogPublish(); assert(lastPublishedEventSequence==12 && !eventSnapshotDirty);
  assert(published.size()==12);
  for(unsigned i=0;i<12;++i) assert(published[i]==i+1);
  MayapCloudInternal::network.connected=true; // Previous PIN-reset case deliberately disconnected STA.
  serviceWifiPowerMode(); assert(radioPs==WIFI_PS_NONE && psWrites==1);
  serviceWifiPowerMode(); assert(psWrites==1);
  radioPs=WIFI_PS_MIN_MODEM; // External radio reinitialization must not fool the cache.
  serviceWifiPowerMode(); assert(radioPs==WIFI_PS_NONE && psWrites==2);
  radioReady=false; serviceWifiPowerMode(); assert(!wifiPowerModeValid);
  radioReady=true; serviceWifiPowerMode(); assert(wifiPowerModeValid && radioPs==WIFI_PS_NONE);
  webSessionActive=false; serviceWifiPowerMode(); assert(radioPs==WIFI_PS_NONE);
  clockMs += 24999; serviceWifiPowerMode(); assert(radioPs==WIFI_PS_NONE);
  webSessionActive=true; serviceWifiPowerMode(); assert(radioPs==WIFI_PS_NONE);
  webSessionActive=false; serviceWifiPowerMode(); clockMs += 25000;
  serviceWifiPowerMode(); assert(radioPs==WIFI_PS_MIN_MODEM);
  const unsigned savedWrites=psWrites; serviceWifiPowerMode(); assert(psWrites==savedWrites);
  radioReady=false; serviceWifiPowerMode(); assert(!wifiPowerModeValid);
  radioReady=true; serviceWifiPowerMode(); assert(wifiPowerModeValid && radioPs==WIFI_PS_MIN_MODEM);
  webSessionActive=true; serviceWifiPowerMode(); assert(radioPs==WIFI_PS_NONE);
  webSessionActive=false; MayapCloudInternal::network.connected=false;
  clockMs+=30000; serviceWifiPowerMode(); assert(radioPs==WIFI_PS_NONE);
  std::puts("Actual stability helpers: TLS/bulk exclusion, admission boundaries, bounded/chunked HTTP, Serial pressure/mute, alarm coalescing, deferred PIN reset and failed log retry OK");
}
