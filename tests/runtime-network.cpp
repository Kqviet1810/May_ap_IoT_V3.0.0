#include <cassert>
#include <cstdint>
#include <cstdio>
#include "../MAYAP_INDUSTRIAL_v4_0_0/runtime_recovery_policy.h"
static uint32_t clockMs=1;
uint32_t millis() { return clockMs; }
void mayapSerialPrintf(bool, const char *, ...) {}
#include "actual-services.inc"
enum class ConnectivityMode : uint8_t { Offline, Online };
enum class NetworkStateCode { Connecting };
constexpr int WIFI_OFF=0, WIFI_STA=1;
constexpr const char *NETWORK_WIFI_HOSTNAME="mayap-test";
static bool portal=false, configured=true;
bool mayapWifiPortalExclusiveRequested() { return portal; }
struct FakeWifi {
  unsigned off=0, sta=0, disconnects=0;
  bool setAutoReconnect(bool enabled) { assert(!enabled); return true; }
  bool disconnect(bool eraseRadio, bool eraseCredentials) { assert(!eraseRadio && !eraseCredentials); ++disconnects; return true; }
  bool mode(int mode) { if (mode==WIFI_OFF) ++off; else { assert(mode==WIFI_STA); ++sta; } return true; }
  bool setHostname(const char *) { return true; }
} WiFi;
namespace MayapNetworkInternal {
static MayapRecovery::WifiRecovery deepPolicy;
enum class DeepPhase : uint8_t { Idle, Quiesce, OffWait, Isolated };
static DeepPhase deepPhase=DeepPhase::Idle;
static uint32_t deepPhaseAt=0;
static bool deepRequested=false, radioActive=true;
static uint8_t requestedMode=static_cast<uint8_t>(ConnectivityMode::Online);
struct Backoff { void reset(uint32_t) {} } staBackoff;
bool credentialsConfigured() { return configured; }
void publish(NetworkStateCode, bool connected) { assert(!connected); }
}
#include "actual-network.inc"
int main() {
  using namespace MayapRecovery;
  Service failed=Service::Network;
  clockMs=1000000;
  assert(!mayapServiceSupervisorUpdate(clockMs,failed)); // Deferred boot tasks ignored.
  mayapServiceAdmit(Service::Mqtt);
  clockMs+=60001;
  assert(!mayapServiceSupervisorUpdate(clockMs,failed));
  assert(mayapServiceRecoveryRequested(Service::Mqtt));
  clockMs+=60000;
  assert(!mayapServiceSupervisorUpdate(clockMs,failed));
  assert(mayapServiceIsolated(Service::Mqtt,clockMs));
  clockMs+=240000;
  assert(mayapServiceSupervisorUpdate(clockMs,failed) && failed==Service::Mqtt);
  mayapServiceRecoveryComplete(Service::Mqtt);
  assert(!mayapServiceSupervisorUpdate(clockMs,failed));
  assert(!mayapServiceIsolated(Service::Mqtt,clockMs));
  // A radio recovery waits for every I/O owner, including an OTA upload.
  mayapSetRadioOtaQuiesced(false);
  mayapRequestWifiDeepRecovery();
  assert(mayapNetworkDeepRecoveryUpdate(clockMs,false));
  assert(mayapRadioRecoveryRequested() && WiFi.off==0);
  assert(mayapNetworkDeepRecoveryUpdate(++clockMs,true) && WiFi.off==0);
  assert(mayapNetworkDeepRecoveryUpdate(++clockMs,false) && WiFi.off==0);
  mayapSetRadioOtaQuiesced(true);
  assert(mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  assert(WiFi.off==1 && WiFi.disconnects==1);
  mayapRequestWifiDeepRecovery(); // Cannot queue an extra cycle while busy.
  clockMs+=499;
  assert(mayapNetworkDeepRecoveryUpdate(clockMs,false) && WiFi.sta==0);
  assert(!mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  assert(WiFi.sta==1 && !mayapRadioRecoveryRequested());
  mayapRequestWifiDeepRecovery();
  assert(!mayapNetworkDeepRecoveryUpdate(++clockMs,false) && WiFi.off==1); // Forced request respects cooldown too.
  // No Internet leaves the task alive; radio attempts are spaced and isolated.
  for (unsigned cycle=0; cycle<2; ++cycle) {
    MayapNetworkInternal::deepPolicy.offline(clockMs);
    clockMs+=WIFI_OFFLINE_MS;
    assert(mayapNetworkDeepRecoveryUpdate(clockMs,false));
    assert(mayapNetworkDeepRecoveryUpdate(++clockMs,false));
    clockMs+=WIFI_OFF_MS;
    const bool isolated=mayapNetworkDeepRecoveryUpdate(clockMs,false);
    assert(isolated == (cycle==1));
  }
  assert(WiFi.off==3 && WiFi.sta==3);
  clockMs+=WIFI_ISOLATE_MS-1;
  assert(mayapNetworkDeepRecoveryUpdate(clockMs,false));
  assert(!mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  // STA needs time to join asynchronously after isolation, before another off.
  assert(!mayapNetworkDeepRecoveryUpdate(++clockMs,false) && WiFi.off==3);
  MayapNetworkInternal::deepPolicy.offline(clockMs);
  clockMs+=30000;
  assert(!mayapNetworkDeepRecoveryUpdate(clockMs,false) && WiFi.off==3);
  MayapNetworkInternal::deepPolicy.success(clockMs);
  portal=true; mayapRequestWifiDeepRecovery();
  assert(!mayapNetworkDeepRecoveryUpdate(++clockMs,false) && WiFi.off==3);
  portal=false; configured=false;
  assert(!mayapNetworkDeepRecoveryUpdate(++clockMs,false) && WiFi.off==3);
  std::puts("Actual service/radio: deferred admission, owner ack, isolation, escalation, busy handshake, OTA quiescence, 500 ms off, cooldown, portal and reconnect PASS");
}
