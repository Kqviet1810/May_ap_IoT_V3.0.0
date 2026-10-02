#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include "../MAYAP_INDUSTRIAL_v4_0_0/runtime_recovery_policy.h"
static uint32_t clockMs=100;
uint32_t millis() { return clockMs; }
void mayapSerialPrintf(bool, const char *, ...) {}
#include "actual-services.inc"
#include "../MAYAP_INDUSTRIAL_v4_0_0/arduino_ota_window.h"
enum esp_reset_reason_t { ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_TASK_WDT };
static esp_reset_reason_t resetReason = ESP_RST_POWERON;
esp_reset_reason_t esp_reset_reason() { return resetReason; }
enum class ConnectivityMode { Offline, Online };
struct NetworkStatus { ConnectivityMode requestedMode=ConnectivityMode::Online; bool connected=true; } network;
NetworkStatus mayapGetNetworkStatus() { return network; }
static bool safe=true, maintenance=false;
bool mayapFirmwareMaintenanceReady() { return safe; }
bool mayapFirmwareMaintenanceActive() { return maintenance; }
void mayapSetFirmwareMaintenanceActive(bool active) { maintenance=active; }
void mayapMarkIntentionalRestart() {}
namespace MayapBoot { enum class RestartReason { ArduinoOta }; }
void mayapBootPlanRestart(MayapBoot::RestartReason, const char *) {}
constexpr const char OTA_PASSWORD[]="test-only";
constexpr const char NETWORK_WIFI_HOSTNAME[]="mayap-test";
constexpr int U_FLASH=0;
enum ota_error_t { OTA_AUTH_ERROR, OTA_BEGIN_ERROR, OTA_CONNECT_ERROR, OTA_RECEIVE_ERROR, OTA_END_ERROR };
struct FakeUpdate { unsigned aborts=0; void abort() { ++aborts; } } Update;
struct FakeOta {
  unsigned begins=0, ends=0, handles=0;
  int getCommand() { return U_FLASH; }
  void end() { ++ends; }
  void begin() { ++begins; }
  void handle() { ++handles; }
  void setHostname(const char *) {}
  void setPassword(const char *) {}
  void onStart(void (*)()) {}
  void onEnd(void (*)()) {}
  void onProgress(void (*)(unsigned,unsigned)) {}
  void onError(void (*)(ota_error_t)) {}
} ArduinoOTA;
#include "actual-ota.inc"
static void boot(esp_reset_reason_t reason = ESP_RST_POWERON, uint32_t now = 100U) {
  resetReason=reason; clockMs=now; safe=true; maintenance=false;
  network=NetworkStatus(); ArduinoOTA=FakeOta(); Update=FakeUpdate();
  MayapOtaInternal::started=false; MayapOtaInternal::inProgress=false;
  MayapOtaInternal::uploadWindow=Mayap::ArduinoOtaWindow();
  mayapOtaBegin(); mayapOtaUpdate(clockMs);
}
int main() {
  mayapServiceAdmit(MayapRecovery::Service::Ota);
  boot(); assert(ArduinoOTA.begins==1);
  MayapOtaInternal::onStart();
  assert(maintenance && mayapOtaInProgress());
  clockMs+=50000;
  assert(!mayapOtaRuntimeRecover(clockMs) && Update.aborts==0);
  assert(!mayapOtaQuiesceForWifiPortal());
  MayapOtaInternal::onProgress(100,200); clockMs+=50000;
  assert(!mayapOtaRuntimeRecover(clockMs));
  clockMs+=10001; assert(mayapOtaRuntimeRecover(clockMs));
  assert(Update.aborts==1 && !maintenance && !mayapOtaInProgress());
  mayapOtaUpdate(clockMs); assert(ArduinoOTA.begins==1); // Attempt consumed.

  boot(ESP_RST_EXT); safe=false; MayapOtaInternal::onStart();
  assert(Update.aborts==1 && !maintenance && !mayapOtaInProgress());
  mayapOtaUpdate(clockMs); assert(!MayapOtaInternal::started);
  safe=true; mayapOtaUpdate(clockMs); assert(ArduinoOTA.begins==2);
  MayapOtaInternal::onError(OTA_AUTH_ERROR); // No authenticated upload yet.
  mayapOtaUpdate(clockMs); assert(MayapOtaInternal::started);
  MayapOtaInternal::onStart(); MayapOtaInternal::onError(OTA_RECEIVE_ERROR);
  mayapOtaUpdate(clockMs); assert(!MayapOtaInternal::started && !maintenance);
  assert(mayapOtaRuntimeRecover(clockMs)); mayapOtaUpdate(clockMs);
  assert(!MayapOtaInternal::started); // Recovery cannot re-arm.

  boot(); network.connected=false; mayapOtaUpdate(clockMs);
  clockMs=Mayap::ArduinoOtaWindow::WINDOW_MS-1U;
  network.connected=true; mayapOtaUpdate(clockMs); assert(MayapOtaInternal::started);
  clockMs++; mayapOtaUpdate(clockMs); assert(!MayapOtaInternal::started);
  const unsigned begins=ArduinoOTA.begins;
  mayapOtaBegin(); network.connected=false; mayapOtaUpdate(clockMs);
  network.connected=true; clockMs=100U; mayapOtaUpdate(clockMs); // Rollover.
  assert(ArduinoOTA.begins==begins);

  boot(ESP_RST_POWERON, Mayap::ArduinoOtaWindow::WINDOW_MS);
  assert(ArduinoOTA.begins==0); // Delayed task admission cannot extend boot time.
  for (auto reason : {ESP_RST_SW, ESP_RST_TASK_WDT}) {
    boot(reason); assert(ArduinoOTA.begins==0);
  }
  boot(ESP_RST_EXT, Mayap::ArduinoOtaWindow::WINDOW_MS-1U);
  MayapOtaInternal::onStart(); clockMs+=2U;
  const unsigned handles=ArduinoOTA.handles;
  mayapOtaUpdate(clockMs); assert(ArduinoOTA.handles==handles+1U && maintenance);
  MayapOtaInternal::onEnd(); mayapOtaUpdate(clockMs);
  assert(!MayapOtaInternal::started && !maintenance);
  boot(ESP_RST_SW); assert(ArduinoOTA.begins==0); // Successful upload's reboot.
  boot(ESP_RST_EXT); assert(ArduinoOTA.begins==1); // New physical authorization.
  std::puts("Actual OTA: physical boot, one attempt, 30 min expiry, rollover, reconnect, recovery, failed auth, safety and active-upload completion PASS");
}
