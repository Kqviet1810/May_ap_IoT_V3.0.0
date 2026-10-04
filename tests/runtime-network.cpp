#include <cassert>
#include <cstdint>
#include <cstdio>
#include "../MAYAP_INDUSTRIAL_v1_0_0/runtime_recovery_policy.h"
static uint32_t clockMs=1;
uint32_t millis() { return clockMs; }
void mayapSerialPrintf(bool, const char *, ...) {}
#include "actual-services.inc"
enum class ConnectivityMode : uint8_t { Offline, Online };
enum class NetworkStateCode { Offline, NotConfigured, Connecting };
constexpr int WIFI_OFF=0, WIFI_STA=1;
constexpr const char *NETWORK_WIFI_HOSTNAME="mayap-test";
static bool portal=false, configured=true;
bool mayapWifiPortalExclusiveRequested() { return portal; }
struct FakeWifi {
  unsigned off=0, sta=0, disconnects=0, reconnects=0, begins=0;
  bool reconnectResult=true;
  bool setAutoReconnect(bool) { return true; }
  bool disconnect(bool eraseRadio, bool eraseCredentials) {
    assert(!eraseRadio && !eraseCredentials); ++disconnects; return true;
  }
  bool mode(int mode) {
    if (mode==WIFI_OFF) ++off;
    else { assert(mode==WIFI_STA); ++sta; }
    return true;
  }
  bool setHostname(const char *) { return true; }
  bool reconnect() { ++reconnects; return reconnectResult; }
  bool begin(const char *, const char *) { ++begins; return true; }
} WiFi;
namespace MayapNetworkInternal {
static MayapRecovery::WifiRecovery deepPolicy;
enum class DeepPhase : uint8_t { Idle, Quiesce, OffWait, Isolated };
static DeepPhase deepPhase=DeepPhase::Idle;
static uint32_t deepPhaseAt=0;
static bool deepRequested=false, radioActive=true;
static bool wifiPowerModeAppliedValid=false;
static uint8_t requestedMode=static_cast<uint8_t>(ConnectivityMode::Online);
static char activeSsid[33]="test";
static char activePassword[65]="pass";
struct Backoff {
  unsigned failures=0;
  void reset(uint32_t) {}
  void onFailure(uint32_t) { ++failures; }
  void onSuccess() {}
} staBackoff;
bool credentialsConfigured() { return configured; }
void publish(NetworkStateCode, bool connected) { assert(!connected); }
void stopRadio() {
  WiFi.setAutoReconnect(false);
  (void)WiFi.disconnect(false,false);
  (void)WiFi.mode(WIFI_OFF);
  radioActive=false;
  wifiPowerModeAppliedValid=false;
}
}
#include "actual-network.inc"
int main() {
  using namespace MayapRecovery;
  clockMs=1000000;
  mayapServiceSupervisorUpdate(clockMs);
  assert(mayapServiceDegradedMask()==0U);
  mayapServiceAdmit(Service::Mqtt);
  clockMs+=60001;
  mayapServiceSupervisorUpdate(clockMs);
  assert(mayapServiceRecoveryRequested(Service::Mqtt));
  clockMs+=60000;
  mayapServiceSupervisorUpdate(clockMs);
  assert(mayapServiceIsolated(Service::Mqtt,clockMs));
  clockMs+=240000;
  mayapServiceSupervisorUpdate(clockMs);
  assert((mayapServiceDegradedMask() & (1U << static_cast<uint8_t>(Service::Mqtt))) != 0U);
  mayapServiceRecoveryComplete(Service::Mqtt);
  assert(mayapServiceDegradedMask()==0U);

  mayapSetRadioOtaQuiesced(false);
  mayapRequestWifiDeepRecovery();
  assert(mayapNetworkDeepRecoveryUpdate(clockMs,false));
  assert(mayapRadioRecoveryRequested() && WiFi.off==0 && WiFi.reconnects==0);
  assert(mayapNetworkDeepRecoveryUpdate(++clockMs,true));
  assert(WiFi.reconnects==0);
  mayapSetRadioOtaQuiesced(true);
  assert(!mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  assert(WiFi.reconnects==1 && WiFi.off==0 && WiFi.disconnects==0);
  assert(!mayapRadioRecoveryRequested());

  mayapRequestWifiDeepRecovery();
  assert(!mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  assert(WiFi.reconnects==1 && WiFi.off==0);
  clockMs += WIFI_COOLDOWN_MS;
  mayapRequestWifiDeepRecovery();
  assert(mayapNetworkDeepRecoveryUpdate(clockMs,false));
  assert(!mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  assert(WiFi.reconnects==2 && WiFi.off==0);

  MayapNetworkInternal::requestedMode=static_cast<uint8_t>(ConnectivityMode::Offline);
  MayapNetworkInternal::radioActive=true;
  assert(mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  assert(mayapRadioRecoveryRequested() && WiFi.off==0);
  assert(mayapNetworkDeepRecoveryUpdate(++clockMs,true));
  assert(WiFi.off==0);
  assert(!mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  assert(WiFi.off==1 && WiFi.disconnects==1 && !MayapNetworkInternal::radioActive);
  assert(!mayapRadioRecoveryRequested());

  MayapNetworkInternal::requestedMode=static_cast<uint8_t>(ConnectivityMode::Online);
  MayapNetworkInternal::radioActive=true;
  portal=true; mayapRequestWifiDeepRecovery();
  assert(!mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  assert(WiFi.off==1 && WiFi.reconnects==2);
  portal=false; configured=false;
  assert(mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  assert(!mayapNetworkDeepRecoveryUpdate(++clockMs,false));
  assert(WiFi.off==2);

  std::puts("Actual service/radio: degraded-not-restart, owner quiesce, safe reconnect, explicit-offline shutdown PASS");
}
