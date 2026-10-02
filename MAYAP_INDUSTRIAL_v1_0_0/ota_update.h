#pragma once

#include "config.h"
#include "boot_diagnostic.h"
#include "service_recovery.h"
#include "firmware_update_guard.h"
#include "arduino_ota_window.h"
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Update.h>

// Nap firmware qua Wi-Fi bang Arduino IDE (Sketch > Upload, chon cong mang
// hien qua mDNS thay vi cong USB) - tien loi khi may da lap dat kin, kho thao
// vo de cam lai cap USB moi lan can cap nhat. Chi mo dich vu khi dang ONLINE
// va da ket noi Wi-Fi that (dua vao mayapGetNetworkStatus() cua network_
// service.h, KHONG tu mo Wi-Fi rieng) - goi mayapOtaUpdate() tu otaTask
// RIENG (xem .ino), KHONG chung voi networkTask: networkTask co the blocking
// toi vai giay moi lan MQTT/Cloud Push lam viec, neu OTA nam chung vong lap
// se co luc khong duoc ArduinoOTA.handle() phan hoi kip, gay loi "No response
// from device" phia Arduino IDE (da gap thuc te, xem lich su sua doi).
//
// Bat buoc mat khau (OTA_PASSWORD trong config.h, dat qua build flag
// MAYAP_OTA_PASSWORD): de trong se TU DONG TAT ca tinh nang nay, tranh mo mot
// cong nap firmware khong xac thuc tren mang LAN.
//
// Luu y an toan: trong luc thuc su ghi tung khoi du lieu vao flash (Update.
// write ben trong ArduinoOTA), ESP-IDF tam dung truy cap cache flash tren CA
// HAI loi CPU trong vai mili giay - anh huong ca controlTask (loi 1). Day la
// gioi han phan cung chung cua moi ung dung OTA tren ESP32, khong rieng gi
// firmware nay. Nen thuc hien nap OTA luc may dang RANH (ngoai me ap) de
// tranh trung hop hiem gap nhieu chu ky dieu khien lien tiep bi cham do cung
// luc voi ghi flash.
namespace MayapOtaInternal {

static bool started = false;
static bool inProgress = false;
static uint32_t lastProgressAt = 0U;
static Mayap::ArduinoOtaWindow uploadWindow;

inline void onStart() {
  if (!mayapFirmwareMaintenanceReady()) {
    Update.abort();
    mayapSerialPrintf(false, "[OTA] MAY CHUA AN TOAN DE CAP NHAT - HUY\n");
    return;
  }
  if (!uploadWindow.consume(millis())) {
    Update.abort();
    mayapSerialPrintf(false, "[OTA] CUA SO NAP DA DONG - CAN RESET VAT LY\n");
    return;
  }
  mayapSetFirmwareMaintenanceActive(true);
  inProgress = true;
  lastProgressAt = millis();
  const char *type = (ArduinoOTA.getCommand() == U_FLASH) ? "chuong trinh" : "he thong tep";
  mayapSerialPrintf(false, "[OTA] Bat dau nap %s qua mang...\n", type);
}

inline void onEnd() {
  inProgress = false;
  mayapSetFirmwareMaintenanceActive(false);
  mayapSerialPrintf(false, "[OTA] Nap xong, chuan bi khoi dong lai\n");
  // Danh dau day la khoi dong lai CO CHU DICH (xem config.h) - thu vien
  // ArduinoOTA tu goi ESP.restart() ngay sau callback nay khi nap thanh
  // cong; khong danh dau se bi PowerManager tinh nham vao bo dem "reset bat
  // thuong" giong het cach ota_web_update.h/ota_rollback.h da xu ly.
  mayapMarkIntentionalRestart();
  mayapBootPlanRestart(MayapBoot::RestartReason::ArduinoOta, "ArduinoOTA complete");
}

inline void onProgress(unsigned int progress, unsigned int total) {
  if (!inProgress) return;
  static uint32_t lastLogAt = 0U;
  const uint32_t now = millis();
  lastProgressAt = now;
  mayapServiceBeat(MayapRecovery::Service::Ota);
  if (lastLogAt != 0U && (now - lastLogAt) < 1000U) return;
  lastLogAt = now;
  const unsigned percent = total ? (progress * 100U) / total : 0U;
  mayapSerialPrintf(false, "[OTA] Da nhan %u%%\n", percent);
}

inline void onError(ota_error_t error) {
  inProgress = false;
  mayapSetFirmwareMaintenanceActive(false);
  const char *reason = "LOI KHONG XAC DINH";
  switch (error) {
    case OTA_AUTH_ERROR: reason = "SAI MAT KHAU"; break;
    case OTA_BEGIN_ERROR: reason = "KHONG MO DUOC VUNG NHO OTA"; break;
    case OTA_CONNECT_ERROR: reason = "MAT KET NOI"; break;
    case OTA_RECEIVE_ERROR: reason = "LOI NHAN DU LIEU"; break;
    case OTA_END_ERROR: reason = "LOI GHI HOAN TAT"; break;
    default: break;
  }
  mayapSerialPrintf(false, "[OTA] THAT BAI: %s (ma %d)\n", reason,
                    static_cast<int>(error));
}

}  // namespace MayapOtaInternal

inline bool mayapOtaEnabled() {
  return sizeof(OTA_PASSWORD) > 1U;  // build flag rong = tinh nang tat
}

inline bool mayapOtaInProgress() {
  return MayapOtaInternal::inProgress;
}

// Goi boi otaTask truoc khi networkTask doi radio sang captive portal.
// Khong bao gio end ArduinoOTA giua mot upload dang chay; portal se cho/timeout.
inline bool mayapOtaQuiesceForWifiPortal() {
  using namespace MayapOtaInternal;
  if (!mayapOtaEnabled()) return true;
  if (inProgress) return false;
  if (started) {
    ArduinoOTA.end();
    started = false;
  }
  return true;
}

// Goi 1 lan trong setup(): chi dang ky cau hinh/callback, CHUA mo cong mang
// nao (an toan goi truoc khi Wi-Fi ket noi, thu tu giong cac module khac).
inline void mayapOtaBegin() {
  if (!mayapOtaEnabled()) return;
  const esp_reset_reason_t reason = esp_reset_reason();
  // EN/reset and power cycling open a window. Automatic OTA/recovery/watchdog
  // restarts do not authorize another upload.
  MayapOtaInternal::uploadWindow.begin(
      reason == ESP_RST_POWERON || reason == ESP_RST_EXT, millis());
  ArduinoOTA.setHostname(NETWORK_WIFI_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart(MayapOtaInternal::onStart);
  ArduinoOTA.onEnd(MayapOtaInternal::onEnd);
  ArduinoOTA.onProgress(MayapOtaInternal::onProgress);
  ArduinoOTA.onError(MayapOtaInternal::onError);
}

// Only the OTA owner may abort/reset its service, never the supervisor task.
// A progressing upload keeps its maintenance interlocks and is not interrupted.
inline bool mayapOtaRuntimeRecover(uint32_t now) {
  using namespace MayapOtaInternal;
  if (inProgress && MayapRecovery::age(now, lastProgressAt) < 60000U) return false;
  if (inProgress) {
    Update.abort();
    inProgress = false;
    mayapSetFirmwareMaintenanceActive(false);
  }
  if (started) ArduinoOTA.end();
  started = false;
  mayapOtaBegin();
  // Internet OTA uses scoped HTTP sessions and an existing 120 s deadline.
  // Its accepted requests, signed metadata and failure UI remain intact.
  return true;
}

// Goi moi chu ky tu networkTask. Chi thuc su bat dich vu OTA (mo UDP/mDNS)
// khi da ket noi Wi-Fi that; tu dong dong lai khi mat mang/chuyen OFFLINE de
// khong giu tai nguyen mang vo ich.
inline void mayapOtaUpdate(uint32_t now) {
  if (!mayapOtaEnabled()) return;
  now = millis();
  const bool windowOpen = MayapOtaInternal::uploadWindow.available(now);

  // Neu upload ArduinoOTA da bat dau, tiep tuc pump cho den khi onEnd/onError.
  // Khong de portal quiescing (publishedConnected=false) cat ngang flash dang ghi.
  if (MayapOtaInternal::inProgress) {
    ArduinoOTA.handle();
    return;
  }

  const NetworkStatus status = mayapGetNetworkStatus();
  const bool shouldRun =
      windowOpen && status.requestedMode == ConnectivityMode::Online && status.connected &&
      mayapFirmwareMaintenanceReady() && !mayapFirmwareMaintenanceActive();

  if (!shouldRun) {
    if (MayapOtaInternal::started) {
      ArduinoOTA.end();
      if (!windowOpen)
        mayapSerialPrintf(false, "[OTA] DONG CONG NAP - HET 30 PHUT HOAC DA DUNG LUOT\n");
      MayapOtaInternal::started = false;
      MayapOtaInternal::inProgress = false;
    }
    return;
  }

  if (!MayapOtaInternal::started) {
    ArduinoOTA.begin();
    MayapOtaInternal::started = true;
    mayapSerialPrintf(false, "[OTA] San sang nhan firmware qua Wi-Fi (%s.local)\n",
                      NETWORK_WIFI_HOSTNAME);
  }
  ArduinoOTA.handle();
}
