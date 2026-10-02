#pragma once

#include "config.h"
#include "service_recovery.h"
#include "network_io_guard.h"
#include "bounded_http.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <string.h>

// ============================================================================
// KENH CANH BAO CLOUD (CLOUDFLARE WORKER) - THAY THE HOAN TOAN TELEGRAM CU
// ----------------------------------------------------------------------------
// KHONG dung chung ket noi/hang doi voi realtime_link.h. File nay tu mo HTTPS
// rieng toi Cloudflare Worker (xem thu muc cloudflare/) - mat MQTT/Web KHONG
// anh huong gi den kenh nay, va nguoc lai. Nguon du lieu goc van la
// MachineController (qua mayapCloudSetRuntime, goi tu controlTask giong het
// pattern mayapWebSetRuntime cua realtime_link.h).
//
// KHAC Telegram truoc day: kenh nay chi MOT CHIEU (ESP32 -> Worker -> Web
// Push -> trinh duyet). Khong con lenh /status, /help hoi nguoc lai ESP32 -
// dieu khien 2 chieu da co san qua MQTT (realtime_link.h), khong can lam lai
// o day. Nguoi dung cuoi KHONG cau hinh gi tren ESP32 cho kenh nay (khong con
// "Chat ID" nhu Telegram) - viec ghep trinh duyet nhan thong bao hoan toan
// thuc hien o trang web (push.js/setup.html), dung device_id cong khai.
//
// Vi tri include: sau hmi.h (dung mayapDeviceIdText tu network_service.h va
// cac kieu MachineRuntime/FaultCode tu machine_control.h/config.h da include
// truoc do), truoc machine_control.h (MachineController goi nguoc lai
// mayapCloudSetRuntime()).
//
// Thu vien can cai: KHONG can cai them - HTTPClient/WiFiClientSecure co san
// trong ESP32 Arduino core; ArduinoJson da la yeu cau cua realtime_link.h.
//
// Tai nguyen: moi lan goi tao MOI mot WiFiClientSecure NGAN HAN (huy ngay sau
// khi xong), khong giu ket noi thuong truc nhu MQTT - phu hop voi tan suat
// thap (vai phut/lan) va tranh chiem RAM lau dai tren thiet bi khong PSRAM.
// TLS bat buoc xac thuc CA goc tu MAYAP_TLS_ROOT_CA; thieu CA thi kenh dong -
// Cloudflare dung chung chi hop le, ESP32 Arduino core khong co san bo goc
// CA de xac thuc day du ma khong tang dang ke dung luong firmware; day la
// danh doi bao mat da duoc ghi nhan, xem bao cao audit.
// ============================================================================

namespace MayapCloudInternal {

inline uint32_t elapsedMs(uint32_t now, uint32_t then) {
  return static_cast<uint32_t>(now - then);
}
inline bool timeReached(uint32_t now, uint32_t target) {
  return static_cast<int32_t>(now - target) >= 0;
}

enum class NotifyLevel : uint8_t { Info, Warning, Critical, System };

inline const char *severityText(NotifyLevel level) {
  switch (level) {
    case NotifyLevel::Critical: return "critical";
    case NotifyLevel::Warning: return "warning";
    case NotifyLevel::System: return "system";
    default: return "info";
  }
}

// ------------------------------ Hop thu runtime --------------------------------
// Ghi boi controlTask (mayapCloudSetRuntime), doc boi networkTask. Copy
// nguyen struct trong critical section ngan, giong het pattern cua
// realtime_link.h::mayapWebSetRuntime - khong I/O trong vung khoa.
static portMUX_TYPE cloudMux = portMUX_INITIALIZER_UNLOCKED;
static MachineRuntime knownRuntime{};
static bool knownRuntimeValid = false;

// Ban lam viec RIENG cua networkTask, cap nhat tu knownRuntime moi chu ky
// kiem tra (duoi khoa, xong roi sao chep ra ngoai). Tat ca ham quyet dinh
// (checkFaults/checkTransitions) chi doc bien nay - khong bao gio bi
// controlTask ghi de, tranh rang buoc "doc-roi-ghi-lai" khong an toan.
static MachineRuntime processingRuntime{};

// Hop thu config (giong het pattern hop thu runtime o tren) - chi can doc
// cac co BAT/TAT canh bao (vd lightAfterBatchAlarmEnabled), khong can toan
// bo MachineConfig nhung dung chung struct cho don gian/de doi chieu.
static MachineConfig knownConfig{};
static bool knownConfigValid = false;
static MachineConfig processingConfig{};

// Danh sach nhac nho tuy chinh (v3.7.0) - cung mailbox pattern, dung cho
// checkCustomReminders() ben duoi bao khi ngay hien tai cham/vuot ngay nguoi
// dung dat cho tung muc.
static ReminderSet knownReminders{};
static bool knownRemindersValid = false;
static ReminderSet processingReminders{};

// Backoff RIENG cho Cloud Push - hoan toan doc lap voi backoff cua MQTT
// (realtime_link.h) va STA Wi-Fi (network_service.h). Dung chung cho ca 3
// loai goi HTTPS (register/heartbeat/alarm) vi ca 3 cung phan anh cung 1 cau
// hoi "co goi duoc toi Worker luc nay khong". Thanh cong o BAT KY chieu nao
// cung reset ve nhanh nhat cho ca 3.
static BackoffTimer cloudBackoff{};
static bool registered = false;

// Co hieu "dat lai ma PIN web ve mac dinh" phat tu HMI (controlTask) toi
// networkTask - dung chung idiom voi portalRequestFlag cua network_service.h
// (volatile + __atomic_*, khong can mutex vi chi 1 writer/1 reader moi
// chieu). Nguoi lap dat co mat vat ly tai HMI la dieu kien DUY NHAT de kich
// hoat - khong co duong nao tu web tu goi duoc lenh nay (endpoint /api/
// device/reset-pin chi chap nhan device_key bi mat cua firmware, khong
// phai PIN web, xem cloudflare/src/index.js::handleResetPin).
static volatile uint8_t pinResetRequestFlag = 0U;

// -------------------------------- Hang doi gui ----------------------------------
// Chi networkTask dung (ca ghi lan doc) - moi logic quyet dinh gui gi cung
// chay trong mayapCloudAlertUpdate(), khong co task nao khac cham vao.
struct OutboxItem {
  bool used = false;
  char alarmType[24] = "";
  NotifyLevel severity = NotifyLevel::Info;
  bool resolved = false;
  char message[160] = "";
  bool hasReadings = false;
  float temperature = 0.0f;
  float humidity = 0.0f;
};
static OutboxItem outbox[CLOUD_OUTBOX_SIZE];
static uint8_t outboxHead = 0U, outboxTail = 0U, outboxCount = 0U;
static uint32_t lastSendAt = 0U;
static uint32_t lastRequestFinishedAt = 0U;
static bool requestDeferred = false;
static uint32_t outboxDropped = 0U, outboxCriticalDropped = 0U;

inline bool enqueueRaw(const char *alarmType, NotifyLevel severity, bool resolved,
                       const char *message, bool hasReadings, float temperature, float humidity) {
  if (!alarmType || !alarmType[0] || !message || !message[0]) return false;
  // Coalesce repeat notifications for the SAME state only. Keep an active
  // alarm and its recovery as distinct events (especially critical faults).
  for (uint8_t n = 0U; n < outboxCount; ++n) {
    const uint8_t p = (outboxTail + CLOUD_OUTBOX_SIZE - 1U - n) % CLOUD_OUTBOX_SIZE;
    OutboxItem &pending = outbox[p];
    if (!pending.used || strcmp(pending.alarmType, alarmType)) continue;
    if (pending.resolved == resolved && pending.severity == severity) {
      snprintf(pending.message, sizeof(pending.message), "%s", message);
      pending.hasReadings = hasReadings; pending.temperature = temperature; pending.humidity = humidity;
      return true;
    }
    break; // Never coalesce across this alarm's opposite transition.
  }
  if (outboxCount >= CLOUD_OUTBOX_SIZE) {
    uint8_t victim = CLOUD_OUTBOX_SIZE;
    for (uint8_t n = 0U; n < outboxCount; ++n) {
      if (outbox[(outboxHead + n) % CLOUD_OUTBOX_SIZE].severity != NotifyLevel::Critical) { victim = n; break; }
    }
    ++outboxDropped;
    if (victim == CLOUD_OUTBOX_SIZE) {
      if (severity != NotifyLevel::Critical) return false;
      victim = 0U; ++outboxCriticalDropped;
    }
    for (uint8_t n = victim; n + 1U < outboxCount; ++n)
      outbox[(outboxHead + n) % CLOUD_OUTBOX_SIZE] = outbox[(outboxHead + n + 1U) % CLOUD_OUTBOX_SIZE];
    outboxTail = (outboxTail + CLOUD_OUTBOX_SIZE - 1U) % CLOUD_OUTBOX_SIZE;
    outbox[outboxTail].used = false;
    --outboxCount;
    mayapSerialPrintf(false, "[CLOUD] outbox loss=%lu critical=%lu\n",
      static_cast<unsigned long>(outboxDropped), static_cast<unsigned long>(outboxCriticalDropped));
  }
  OutboxItem &item = outbox[outboxTail];
  snprintf(item.alarmType, sizeof(item.alarmType), "%s", alarmType);
  item.severity = severity;
  item.resolved = resolved;
  snprintf(item.message, sizeof(item.message), "%s", message);
  item.hasReadings = hasReadings;
  item.temperature = temperature;
  item.humidity = humidity;
  item.used = true;
  outboxTail = static_cast<uint8_t>((outboxTail + 1U) % CLOUD_OUTBOX_SIZE);
  ++outboxCount;
  return true;
}

inline void enqueueLevel(const char *alarmType, NotifyLevel level, const char *body) {
  const bool hasReadings = knownRuntimeValid;
  enqueueRaw(alarmType, level, false, body, hasReadings, processingRuntime.temperature, processingRuntime.humidity);
}

inline void enqueueResolved(const char *alarmType, NotifyLevel level, const char *body) {
  const bool hasReadings = knownRuntimeValid;
  enqueueRaw(alarmType, level, true, body, hasReadings, processingRuntime.temperature, processingRuntime.humidity);
}

// --------------------------- Noi dung loi (Vietnamese) --------------------------
inline const char *faultSummaryText(uint16_t code) {
  switch (code) {
    case 101: return "Mất cảm biến nhiệt độ/độ ẩm";
    case 102: return "Cảm biến trả về giá trị sai";
    case 103: return "Cảm biến bất thường (nghi ngờ hỏng)";
    case 104: return "Cảm biến đứng giá khi heater vẫn cấp nhiệt";
    case 110: return "Nhiệt độ xuống thấp hơn ngưỡng cảnh báo";
    case 111: return "Nhiệt độ vượt quá ngưỡng cảnh báo cao";
    case 112: return "QUÁ NHIỆT KHẨN CẤP - đã ngắt nguồn nhiệt ngay lập tức";
    case 113: return "Nhiệt độ biến thiên bất thường - kiểm tra quạt/relay";
    case 114: return "Nhiệt độ dao động bất thường - kiểm tra chỉnh định PID";
    case 115: return "Thanh nhiệt hoạt động nhưng nhiệt không tăng - nghi ngờ hỏng relay/SSR";
    case 120: return "Độ ẩm thấp - kiểm tra nguồn cấp nước";
    case 121: return "Độ ẩm cao bất thường - kiểm tra thông gió";
    case 130: return "Công tắc nhiệt bị tắt trong lúc đang ấp";
    case 137: return "Đang chờ RTC hợp lệ để chạy tiếp mẻ ấp - không xác định được đã mất điện bao lâu";
    case 132: return "Cần bật lại chế độ AUTO để tiếp tục mẻ";
    case 133: return "Chế độ AUTO bị tắt trong lúc đang ấp";
    case 134: return "Đảo trứng tự động bị khóa trong lúc đang ấp";
    case 201: return "Lỗi cả 2 công tắc hành trình đảo trứng";
    case 202: return "Đảo trứng quá thời gian cho phép";
    case 203: return "Cơ cấu đảo trứng bị kẹt";
    case 204: return "Xung đột lệnh điều khiển đảo trứng";
    case 301: return "Mất dữ liệu cấu hình EEPROM";
    case 302: return "Bộ nhớ EEPROM có dấu hiệu suy giảm";
    case 303: return "Máy vừa khởi động lại bất thường";
    case 304: return "Xung đột tín hiệu điều khiển đầu ra";
    case 305: return "Relay đóng cắt quá nhiều lần trong giờ";
    case 306: return "Lỗi đồng hồ thời gian thực (RTC)";
    case 313: return "Đang dọn dẹp dữ liệu mẻ cũ trước đó";
    case 314: return "Mất nhật ký an toàn (safety journal)";
    case 315: return "Mất nhật ký sự kiện của mẻ";
    case 401: return "Bộ nhớ RAM còn thấp - hệ thống đang tự theo dõi";
    case 402: return "Bộ nhớ RAM cạn kiệt - máy sẽ tự khởi động lại để phòng ngừa";
    case 403: return "Dự đoán nhiệt độ sắp chạm ngưỡng cảnh báo theo tốc độ hiện tại";
    case 404: return "Bộ nhớ EEPROM phải thử lại nhiều bất thường - có thể đang suy giảm";
    case 501: return "Mất liên lạc mạch báo mất điện ATtiny - kiểm tra pin CR2032 và dây tín hiệu";
    case 502: return "Pin còi 9V sắp hết - hãy thay pin sớm để bảo đảm còi báo khi mất điện";
    case 503: return "Trạng thái mẻ giữa ESP32 và ATtiny chưa đồng bộ";
    default: return "Lỗi không xác định";
  }
}

// HmiFaultItem.severity la uint8_t "tho" (khong phai enum Mayap::FaultSeverity)
// chinh vi ly do nay: config.h dung truoc machine_control.h trong thu tu
// include, va cloud_alert_link.h cung dung truoc machine_control.h nen KHONG
// the tham chieu toi kieu Mayap::FaultSeverity (chi ton tai sau khi
// machine_control.h duoc doc). Dung thang gia tri so, khop dinh nghia enum
// {Info=0, Warning=1, Stop=2, Emergency=3} trong machine_control.h.
constexpr uint8_t FAULT_SEVERITY_INFO = 0U;
constexpr uint8_t FAULT_SEVERITY_WARNING = 1U;
constexpr uint8_t FAULT_SEVERITY_STOP = 2U;
constexpr uint8_t FAULT_SEVERITY_EMERGENCY = 3U;

inline NotifyLevel levelForSeverity(uint8_t severity) {
  if (severity == FAULT_SEVERITY_EMERGENCY || severity == FAULT_SEVERITY_STOP) {
    return NotifyLevel::Critical;
  }
  if (severity == FAULT_SEVERITY_WARNING) return NotifyLevel::Warning;
  return NotifyLevel::Info;
}

inline uint32_t repeatIntervalForSeverity(uint8_t severity) {
  if (severity == FAULT_SEVERITY_EMERGENCY) return CLOUD_REPEAT_CRITICAL_EMERGENCY_MS;
  if (severity == FAULT_SEVERITY_STOP) return CLOUD_REPEAT_CRITICAL_STOP_MS;
  if (severity == FAULT_SEVERITY_WARNING) return CLOUD_REPEAT_WARNING_MS;
  return CLOUD_REPEAT_INFO_MS;
}

// ------------------------- Theo doi loi dang xay ra ------------------------------
// Rieng cho Cloud Push, doc lap voi eventLog_/HMI: chi quan tam "dieu kien co
// con that su xay ra hay khong" (flags bit0) de quyet dinh gui moi/nhac
// lai/da het, khong quan tam co ACK tren HMI hay chua.
struct FaultTrack {
  bool used = false;
  uint16_t code = 0;
  uint8_t severity = 0;
  uint32_t firstSentAt = 0;
  uint32_t lastSentAt = 0;
};
static FaultTrack faultTrack[CLOUD_ACTIVE_TRACK_SIZE];

inline void alarmTypeForFault(uint16_t code, char *out, size_t outLen) {
  snprintf(out, outLen, "FAULT_%u", code);
}

inline void checkFaults(uint32_t now) {
  bool seen[CLOUD_ACTIVE_TRACK_SIZE]{};
  const uint8_t count = processingRuntime.activeFaultDisplayCount;
  for (uint8_t i = 0; i < count; ++i) {
    const HmiFaultItem &item = processingRuntime.activeFaults[i];
    const bool conditionLive = (item.flags & 0x01U) != 0U;
    if (!conditionLive) continue;

    int16_t slot = -1;
    for (uint8_t s = 0; s < CLOUD_ACTIVE_TRACK_SIZE; ++s) {
      if (faultTrack[s].used && faultTrack[s].code == item.code) { slot = static_cast<int16_t>(s); break; }
    }
    if (slot < 0) {
      for (uint8_t s = 0; s < CLOUD_ACTIVE_TRACK_SIZE; ++s) {
        if (!faultTrack[s].used) { slot = static_cast<int16_t>(s); break; }
      }
    }
    if (slot < 0) continue;  // bang theo doi day (rat hiem) - bo qua ky nay

    seen[slot] = true;
    FaultTrack &track = faultTrack[static_cast<uint8_t>(slot)];
    const NotifyLevel level = levelForSeverity(item.severity);
    const uint32_t interval = repeatIntervalForSeverity(item.severity);
    char alarmType[24];
    alarmTypeForFault(item.code, alarmType, sizeof(alarmType));
    char body[160];

    if (!track.used) {
      track.used = true;
      track.code = item.code;
      track.severity = item.severity;
      track.firstSentAt = now;
      track.lastSentAt = now;
      // KHONG kem "ma loi X" - nguoi dung thuong khong can biet ma noi bo,
      // chi can biet DANG XAY RA CHUYEN GI (xem faultSummaryText).
      snprintf(body, sizeof(body), "%s", faultSummaryText(item.code));
      enqueueLevel(alarmType, level, body);
    } else if (timeReached(now, track.lastSentAt + interval)) {
      track.lastSentAt = now;
      snprintf(body, sizeof(body), "Vẫn còn: %s", faultSummaryText(item.code));
      enqueueLevel(alarmType, level, body);
    }
  }

  for (uint8_t s = 0; s < CLOUD_ACTIVE_TRACK_SIZE; ++s) {
    if (faultTrack[s].used && !seen[s]) {
      char alarmType[24];
      alarmTypeForFault(faultTrack[s].code, alarmType, sizeof(alarmType));
      char body[160];
      // Bao ro rang la DA HET (khac han luc moi bao - xem enqueueLevel o
      // tren), khong chi lap lai y y mo ta loi kem "ma loi X" nhu truoc -
      // nguoi dung de nham la dang bao lai loi cu chu khong phai da het.
      // "Da het:" dung DAU cau, khong phai cuoi - nguoi dung thuong chi doc
      // vai chu dau tien cua thong bao, can biet NGAY la loi da qua chua
      // phai doc het ca mo ta loi cu roi moi thay chu "da khoi phuc" o cuoi.
      snprintf(body, sizeof(body), "Đã hết: %s", faultSummaryText(faultTrack[s].code));
      enqueueResolved(alarmType, levelForSeverity(faultTrack[s].severity), body);
      faultTrack[s] = FaultTrack{};
    }
  }
}

// --------------------------- Su kien mot lan (INFO/SYSTEM) -----------------------
static bool lastBatchRunning = false;
static bool haveLastBatchRunning = false;

inline void checkTransitions(uint32_t now) {
  (void)now;
  if (!haveLastBatchRunning) {
    lastBatchRunning = processingRuntime.batchRunning;
    haveLastBatchRunning = true;
  } else if (processingRuntime.batchRunning != lastBatchRunning) {
    lastBatchRunning = processingRuntime.batchRunning;
    enqueueLevel(processingRuntime.batchRunning ? "BATCH_STARTED" : "BATCH_ENDED", NotifyLevel::Info,
        processingRuntime.batchRunning ? "Đã bắt đầu mẻ ấp mới." : "Đã kết thúc mẻ ấp.");
  }
}

// ------------------- Thong bao: da co dien lai giua me ap -------------------
// Khi mat dien, chinh may ap cung tat theo nen KHONG the tu bao luc do (canh
// bao "mat ket noi" do Worker tu phat hien qua khoang lang heartbeat - xem
// checkDeviceConnectivity trong cloudflare/src/index.js). Nhung luc CO DIEN
// LAI thi ESP32 song lai va biet ro minh vua khoi dong sau mat dien giua me
// (runtime.powerLossRecovery, dat trong MachineController::begin) - day la
// thoi diem bao ve dien thoai chinh xac va co ich nhat: nguoi dung can biet
// dien da co lai VA me ap co tu chay tiep khong hay dang cho xac nhan tay.
static bool powerRestoreReported = false;

inline void checkPowerRestored(uint32_t now) {
  (void)now;
  if (powerRestoreReported || !processingRuntime.powerLossRecovery) return;
  // Chi bao khi thuc su co me ap dang cho phuc hoi/dang chay - mat dien luc
  // khong ap gi thi khong can lam phien (cung nguyen tac voi canh bao mat
  // ket noi phia Worker, chi bao khi dang co me).
  if (!processingRuntime.batchRunning && !processingRuntime.resumeConfirmationRequired) return;
  powerRestoreReported = true;
  if (processingRuntime.resumeConfirmationRequired) {
    enqueueLevel("POWER_RESTORED", NotifyLevel::Warning,
        "Đã có điện lại. Mẻ ấp đang CHỜ XÁC NHẬN trên máy để chạy tiếp.");
  } else {
    char body[160];
    snprintf(body, sizeof(body),
        "Đã có điện lại. Mẻ ấp đã tự chạy tiếp (ngày %u/%u).",
        static_cast<unsigned>(processingRuntime.currentDay),
        static_cast<unsigned>(processingConfig.totalIncubationDays));
    enqueueLevel("POWER_RESTORED", NotifyLevel::Info, body);
  }
}

// --------------------- Canh bao: den van bat khi dang ap me --------------------
// Dieu kien: me ap dang chay VA den (lightOn) van bat. Gui 1 lan khi vua phat
// hien, sau do nhac lai moi CLOUD_LIGHT_AFTER_BATCH_REPEAT_MS (30 phut) neu
// van con dung, va bao "da binh thuong" ngay khi het dieu kien (tat den hoac
// ket thuc me) - dung nguyen mau checkFaults() nhung cho 1 dieu kien don, co
// the tat rieng qua config (khac cac loi FaultCode khac khong tat duoc).
static bool lightAfterBatchActive = false;
static uint32_t lightAfterBatchLastSentAt = 0;

inline void checkLightAfterBatch(uint32_t now) {
  if (!processingConfig.lightAfterBatchAlarmEnabled) {
    lightAfterBatchActive = false;  // nguoi dung vua tat: khong con "dinh" trang thai active cu
    return;
  }
  const bool condition = processingRuntime.batchRunning && processingRuntime.lightOn;
  if (condition) {
    if (!lightAfterBatchActive) {
      lightAfterBatchActive = true;
      lightAfterBatchLastSentAt = now;
      enqueueLevel("LIGHT_ON_DURING_BATCH", NotifyLevel::Warning,
          "Đèn đang bật trong lúc mẻ ấp đang chạy - kiểm tra nếu không cần thiết.");
    } else if (timeReached(now, lightAfterBatchLastSentAt + CLOUD_LIGHT_AFTER_BATCH_REPEAT_MS)) {
      lightAfterBatchLastSentAt = now;
      enqueueLevel("LIGHT_ON_DURING_BATCH", NotifyLevel::Warning,
          "Vẫn còn: đèn đang bật trong lúc mẻ ấp đang chạy.");
    }
  } else if (lightAfterBatchActive) {
    lightAfterBatchActive = false;
    // Noi RO nguyen nhan het canh bao, khong bao chung chung "den da tat HOAC
    // me ap da ket thuc" - nguoi dung doc xong khong biet thuc te vua xay ra
    // chuyen gi. Tai day van con du du lieu de biet chinh xac ve nao dung.
    if (!processingRuntime.lightOn) {
      enqueueResolved("LIGHT_ON_DURING_BATCH", NotifyLevel::Warning,
          "Đã hết: đèn đã được tắt.");
    } else {
      enqueueResolved("LIGHT_ON_DURING_BATCH", NotifyLevel::Warning,
          "Đã hết: mẻ ấp đã kết thúc (đèn vẫn đang bật).");
    }
  }
}

// --------------------- Canh bao: bo lo lich dao trung ---------------------
// Khac cac loi co khi tuc thi da co (ket CTHT, qua gio dao...): day la "lich
// dao bi treo am tham" - dem so lan dao THANH CONG (turnCountBatch) khong
// tang du lau so voi chu ky da cau hinh, du khong co loi co khi ro rang nao.
static uint32_t turnMissedLastCount = 0;
static bool turnMissedHaveCount = false;
static uint32_t turnMissedCountChangedAt = 0;
static bool turnMissedActive = false;

inline void checkTurnCycleMissed(uint32_t now) {
  if (!processingConfig.turningEnabled || !processingRuntime.batchRunning) {
    turnMissedHaveCount = false;
    if (turnMissedActive) {
      turnMissedActive = false;
      // KHONG bao "da hoat dong binh thuong tro lai" o day - canh bao het
      // vi me ap dung/nguoi dung tat tu dong dao, KHONG phai vi co cau dao
      // da chay lai duoc. Bao dung su that de nguoi dung khong hieu nham la
      // may da tu khac phuc xong (nhanh "da chay lai that" nam ben duoi).
      if (!processingRuntime.batchRunning) {
        enqueueResolved("TURN_CYCLE_STALLED", NotifyLevel::Warning,
            "Đã hết: mẻ ấp đã kết thúc (chưa kiểm tra được cơ cấu đảo).");
      } else {
        enqueueResolved("TURN_CYCLE_STALLED", NotifyLevel::Warning,
            "Đã hết: đã tắt tự động đảo (chưa kiểm tra được cơ cấu đảo).");
      }
    }
    return;
  }
  if (!turnMissedHaveCount || processingRuntime.turnCountBatch != turnMissedLastCount) {
    turnMissedLastCount = processingRuntime.turnCountBatch;
    turnMissedCountChangedAt = now;
    turnMissedHaveCount = true;
    if (turnMissedActive) {
      turnMissedActive = false;
      // Day moi la phuc hoi THAT: dem so lan dao thanh cong vua tang tro lai.
      enqueueResolved("TURN_CYCLE_STALLED", NotifyLevel::Warning,
          "Đã hết: đảo trứng đã chạy lại bình thường.");
    }
    return;
  }
  const uint32_t staleLimitMs = static_cast<uint32_t>(processingConfig.turnIntervalMin) *
      60000UL * TURN_MISSED_MULTIPLIER;
  if (!turnMissedActive && staleLimitMs > 0U &&
      timeReached(now, turnMissedCountChangedAt + staleLimitMs)) {
    turnMissedActive = true;
    enqueueLevel("TURN_CYCLE_STALLED", NotifyLevel::Warning,
        "Không ghi nhận đảo trứng thành công quá lâu - kiểm tra cơ cấu đảo.");
  }
}

// --------------------- Nhac: sap den ngay no / me qua han ------------------
static bool batchNearingEndSent = false;
static bool batchOverdueActive = false;

inline void checkBatchSchedule(uint32_t now) {
  (void)now;
  if (!processingRuntime.batchRunning) {
    batchNearingEndSent = false;
    if (batchOverdueActive) {
      batchOverdueActive = false;
      enqueueResolved("BATCH_OVERDUE", NotifyLevel::Info, "Mẻ ấp đã kết thúc.");
    }
    return;
  }
  const uint8_t total = processingConfig.totalIncubationDays;
  const uint8_t current = processingRuntime.currentDay;
  if (total == 0U) return;

  if (!batchNearingEndSent && total > current &&
      static_cast<uint8_t>(total - current) <= BATCH_NEARING_END_DAYS_LEFT) {
    batchNearingEndSent = true;
    char body[160];
    snprintf(body, sizeof(body),
        "Còn %u ngày đến ngày dự kiến nở (ngày %u/%u).",
        static_cast<unsigned>(total - current), static_cast<unsigned>(current),
        static_cast<unsigned>(total));
    enqueueLevel("BATCH_NEARING_END", NotifyLevel::Info, body);
  }
  if (!batchOverdueActive && current > total) {
    batchOverdueActive = true;
    char body[160];
    snprintf(body, sizeof(body),
        "Quá hạn %u ngày (ngày %u/%u) - kiểm tra tình trạng trứng.",
        static_cast<unsigned>(current - total), static_cast<unsigned>(current),
        static_cast<unsigned>(total));
    enqueueLevel("BATCH_OVERDUE", NotifyLevel::Warning, body);
  }
}

// --------------- Nhac nho tuy chinh (v3.7.0, nguoi dung tu tao) --------------
// Web da lam TOAN BO viec "phan tich" (hieu cau nhap tu nhien, loc trung/
// khong hop le tren form) - o day CHI can 1 viec don gian: ngay hien tai
// (processingRuntime.currentDay) cham/vuot ngay da dat cho tung muc thi bao 1
// LAN. Phan biet "me MOI thuc su bat dau" (cho phep bao ngay ca ngay 1, neu
// nguoi dung dat nhac "ngay 1") voi "vua khoi dong lai giua 1 me dang chay"
// (OTA/health-restart/mat dien...) - truong hop sau phai dat "co so" theo
// ngay hien tai truoc, KHONG bao dong loat cac muc da qua ngay tu truoc do.
static bool reminderFired[MAX_CUSTOM_REMINDERS] = {};
static bool reminderLastBatchRunning = false;
static bool reminderHaveLastBatchRunning = false;

inline void checkCustomReminders(uint32_t now) {
  (void)now;
  const bool running = processingRuntime.batchRunning;

  if (!reminderHaveLastBatchRunning) {
    reminderHaveLastBatchRunning = true;
    reminderLastBatchRunning = running;
    if (running) {
      const uint8_t current = processingRuntime.currentDay;
      for (uint8_t i = 0; i < MAX_CUSTOM_REMINDERS; ++i) {
        reminderFired[i] = current >= processingReminders.items[i].day;
      }
    } else {
      for (bool &fired : reminderFired) fired = false;
    }
    return;
  }

  if (running && !reminderLastBatchRunning) {
    // Me MOI thuc su bat dau (KHONG-CHAY -> DANG-CHAY trong cung phien nguon
    // dien nay) - reset toan bo de nhac "ngay 1" van bao duoc ngay hom nay.
    for (bool &fired : reminderFired) fired = false;
  }
  reminderLastBatchRunning = running;
  if (!running) return;

  const uint8_t current = processingRuntime.currentDay;
  for (uint8_t i = 0; i < MAX_CUSTOM_REMINDERS; ++i) {
    const uint8_t day = processingReminders.items[i].day;
    if (day == 0U || reminderFired[i]) continue;
    if (current < day) continue;
    reminderFired[i] = true;
    char alarmType[24];
    snprintf(alarmType, sizeof(alarmType), "REMINDER_%u", static_cast<unsigned>(i));
    char body[160];
    const char *label = processingReminders.items[i].label;
    snprintf(body, sizeof(body), "Nhắc ngày %u: %s", static_cast<unsigned>(day),
             label[0] ? label : "(không có tên)");
    enqueueLevel(alarmType, NotifyLevel::Info, body);
  }
}

// ------------------------- Canh bao: Wi-Fi tin hieu yeu ---------------------
static bool wifiWeakTracking = false;
static uint32_t wifiWeakSinceAt = 0;
static bool wifiWeakActive = false;

inline void checkWifiSignal(uint32_t now) {
  const NetworkStatus status = mayapGetNetworkStatus();
  const bool onlineAndWeak = status.requestedMode == ConnectivityMode::Online &&
      status.connected && status.rssiDbm <= WIFI_RSSI_WEAK_DBM;
  if (!onlineAndWeak) {
    wifiWeakTracking = false;
    if (wifiWeakActive) {
      wifiWeakActive = false;
      enqueueResolved("WIFI_SIGNAL_WEAK", NotifyLevel::Info, "Đã hết: tín hiệu Wi-Fi đã ổn định trở lại.");
    }
    return;
  }
  if (!wifiWeakTracking) {
    wifiWeakTracking = true;
    wifiWeakSinceAt = now;
    return;
  }
  if (!wifiWeakActive && timeReached(now, wifiWeakSinceAt + WIFI_RSSI_WEAK_DURATION_MS)) {
    wifiWeakActive = true;
    char body[160];
    snprintf(body, sizeof(body),
        "Tín hiệu Wi-Fi yếu kéo dài (%d dBm) - nên đặt máy gần router hơn.",
        static_cast<int>(status.rssiDbm));
    enqueueLevel("WIFI_SIGNAL_WEAK", NotifyLevel::Warning, body);
  }
}

// GHI CHU: tung co checkConnectivity() gui "SYSTEM_ONLINE/SYSTEM_OFFLINE" moi
// khi ESP32 tu thay doi trang thai mang - BO DI vi qua on ao (tu bao ngay ca
// khi WiFi chi giat rat ngan luc dang backoff/thu lai) va da THUA so voi canh
// bao "mat ket noi thiet bi" phia Worker (checkDeviceConnectivity trong
// cloudflare/src/index.js) - kenh do doc lap, co debounce that su (>=2 phut),
// dang tin cay hon nhieu. Trang thai online/offline tuc thi van xem duoc tren
// web qua MQTT (khong can push rieng).

// ------------------------------- Goi HTTPS ---------------------------------------
inline bool beginCloudRequest(HTTPClient &http, WiFiClientSecure &client, const char *path) {
  if (!TLS_ROOT_CA[0]) {
    mayapSetProvisioningState(MayapProvisioningState::TlsError);
    mayapSerialPrintf(false, "[CLOUD] TLS bi khoa: thieu CA goc tin cay\n");
    return false;
  }
  client.setCACert(TLS_ROOT_CA);
  client.setConnectionTimeout(CLOUD_HTTP_CONNECT_TIMEOUT_MS);
  client.setHandshakeTimeout(8);
  http.setConnectTimeout(CLOUD_HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
  char url[160];
  snprintf(url, sizeof(url), "https://%s%s", CLOUD_API_HOST, path);
  const bool started = http.begin(client, url);
  if (!started) mayapSetProvisioningState(MayapProvisioningState::CloudError);
  return started;
}

inline bool postJson(const char *path, const JsonDocument &doc, const char *logTag,
                     String *responseBody = nullptr, int *responseCode = nullptr) {
  requestDeferred = true;
  if (lastRequestFinishedAt != 0U &&
      elapsedMs(millis(), lastRequestFinishedAt) < CLOUD_MIN_SEND_GAP_MS) {
    if (responseCode) *responseCode = 0;
    return false;
  }
  MayapTlsOperation tlsOperation(MayapTlsKind::Cloud);
  if (!tlsOperation) {
    if (responseCode) *responseCode = 0;
    return false; // defer through the existing bounded Cloud retry queue
  }
  requestDeferred = false;
  const uint32_t heapBefore = ESP.getFreeHeap();
  const uint32_t startedAt = millis();
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginCloudRequest(http, client, path)) {
    http.end();
    client.stop();
    if (responseCode) *responseCode = 0;
    mayapSerialPrintf(false, "[CLOUD] %s -> http.begin() THAT BAI (URL/TLS)\n", logTag);
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  char body[1024];
  const size_t bodySize = measureJson(doc);
  if (doc.overflowed() || bodySize >= sizeof(body)) {
    http.end(); client.stop();
    lastRequestFinishedAt = millis();
    if (responseCode) *responseCode = 0;
    return false;
  }
  serializeJson(doc, body, sizeof(body));
  const int code = http.POST(reinterpret_cast<uint8_t *>(body), bodySize);
  if (responseCode) *responseCode = code;
  char response[1024]{};
  const bool bodyOk = code > 0 && mayapReadBoundedHttpBody(http, response, sizeof(response));
  const bool ok = code == 200 && bodyOk;
  if (responseBody) *responseBody = bodyOk ? response : "";
  if (ok) {
    mayapSerialPrintf(false, "[CLOUD] %s -> HTTP 200 OK\n", logTag);
  } else {
    // Never print server response content (provisioning may contain secrets).
    mayapSerialPrintf(false, "[CLOUD] %s -> HTTP %d FAIL bodyOk=%u\n", logTag, code, bodyOk);
  }
  http.end();
  client.stop();
  lastRequestFinishedAt = millis();
  mayapSerialPrintf(false, "[TLS] cloud=%s ms=%lu heapBefore=%lu after=%lu minEver=%lu\n",
      logTag, static_cast<unsigned long>(elapsedMs(millis(), startedAt)),
      static_cast<unsigned long>(heapBefore), static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(ESP.getMinFreeHeap()));
  mayapServiceBeat(MayapRecovery::Service::Cloud);
  return ok;
}

inline void storeProvisioningFromResponse(const String &response) {
  JsonDocument parsed;
  if (deserializeJson(parsed, response)) return;
  if (!parsed["success"].as<bool>()) return;
  // Worker chi tra web_pin cho may moi, may vua reset PIN, hoac ban ghi cu
  // chua co PIN. May cu da co PIN phai giu PIN cu; danh dau da dong bo de
  // HMI khong hien "DANG DONG BO" vo han.
  mayapMarkWebPinConfigured();
  const char *pin = parsed["web_pin"] | "";
  if (pin[0]) mayapStoreWebPin(pin);
  const char *commandKey = parsed["command_key"] | "";
  if (commandKey[0]) {
    if (!mayapStoreCommandKey(commandKey)) {
      mayapSerialPrintf(false, "[CLOUD] command_key khong hop le/khong luu duoc\n");
    }
  }
}

inline bool rotateLegacyDeviceKey() {
  if (!mayapDeviceUsingLegacySecret()) return true;
  char newKey[65];
  mayapGenerateDeviceSecret(newKey);
  JsonDocument doc;
  doc["device_id"] = mayapDeviceIdText();
  doc["device_key"] = mayapDeviceSecret();
  doc["new_device_key"] = newKey;
  if (!postJson("/api/device/rotate-key", doc, "rotate-key")) return false;
  return mayapCommitDeviceSecret(newKey);
}

inline bool sendRegister() {
  mayapSetProvisioningState(MayapProvisioningState::Syncing);
  JsonDocument doc;
  doc["device_id"] = mayapDeviceIdText();
  doc["device_key"] = mayapDeviceSecret();
  doc["device_name"] = mayapDeviceIdText();
  String response;
  int code = 0;
  if (!postJson("/api/device/register", doc, "register", &response, &code)) {
    if (code == 403) {
      mayapSetProvisioningState(MayapProvisioningState::ServerDenied);
    } else if (code == 401) {
      mayapSetProvisioningState(MayapProvisioningState::KeyMismatch);
    } else if (code != 0) {
      mayapSetProvisioningState(MayapProvisioningState::CloudError);
    }
    return false;
  }
  storeProvisioningFromResponse(response);
  return rotateLegacyDeviceKey();
}

// Tao PIN web ngau nhien moi. PIN duoc tra ve qua TLS, luu vao NVS va hien
// tren man Thong tin ket noi; khong con quay ve gia tri mac dinh.
inline bool sendResetPin() {
  JsonDocument doc;
  doc["device_id"] = mayapDeviceIdText();
  doc["device_key"] = mayapDeviceSecret();
  String response;
  int code = 0;
  if (!postJson("/api/device/reset-pin", doc, "reset-pin", &response, &code)) {
    if (code == 403) {
      mayapSetProvisioningState(MayapProvisioningState::ServerDenied);
    } else if (code == 401) {
      mayapSetProvisioningState(MayapProvisioningState::KeyMismatch);
    } else if (code != 0) {
      mayapSetProvisioningState(MayapProvisioningState::CloudError);
    }
    return false;
  }
  storeProvisioningFromResponse(response);
  return true;
}

inline bool sendHeartbeat() {
  JsonDocument doc;
  doc["device_id"] = mayapDeviceIdText();
  doc["device_key"] = mayapDeviceSecret();
  // Worker dung co nay de quyet dinh co bao "mat ket noi" hay khong - chi bao
  // khi dang co me ap chay tai lan heartbeat gan nhat (xem checkDeviceConnectivity
  // trong cloudflare/src/index.js). processingRuntime duoc lam moi moi chu ky
  // kiem tra (CLOUD_CHECK_INTERVAL_MS), du moi cho heartbeat moi 30s.
  doc["batch_running"] = processingRuntime.batchRunning;
  return postJson("/api/device/heartbeat", doc, "heartbeat");
}

inline bool sendAlarm(const OutboxItem &item) {
  JsonDocument doc;
  doc["device_id"] = mayapDeviceIdText();
  doc["device_key"] = mayapDeviceSecret();
  doc["alarm_type"] = item.alarmType;
  doc["severity"] = severityText(item.severity);
  doc["state"] = item.resolved ? "resolved" : "active";
  doc["message"] = item.message;
  if (item.hasReadings) {
    doc["temperature"] = item.temperature;
    doc["humidity"] = item.humidity;
  }
  return postJson("/api/device/alarm", doc, "alarm");
}

inline void drainOutbox(uint32_t now) {
  if (outboxCount == 0U) return;
  if (!timeReached(now, lastSendAt + CLOUD_MIN_SEND_GAP_MS)) return;
  if (!cloudBackoff.ready(now)) return;  // lan goi truoc vua that bai
  const NetworkStatus status = mayapGetNetworkStatus();
  if (!(status.requestedMode == ConnectivityMode::Online && status.connected)) return;

  lastSendAt = now;
  OutboxItem &item = outbox[outboxHead];
  const bool ok = sendAlarm(item);
  if (ok) {
    cloudBackoff.onSuccess();
    item.used = false;
    outboxHead = static_cast<uint8_t>((outboxHead + 1U) % CLOUD_OUTBOX_SIZE);
    --outboxCount;
  } else {
    // That bai (mang chap chon, Worker loi tam thoi...): giu nguyen dau hang
    // doi (khong mat tin), nhung lui backoff truoc khi cho phep thu lai -
    // khong dap HTTPS lien tuc moi CLOUD_MIN_SEND_GAP_MS trong khi mang dang
    // that su mat trong nhieu gio.
    if (!requestDeferred) cloudBackoff.onFailure(millis());
  }
}

static uint32_t lastHeartbeatAt = 0U;

inline void serviceHeartbeat(uint32_t now) {
  if (!timeReached(now, lastHeartbeatAt + CLOUD_HEARTBEAT_INTERVAL_MS)) return;
  if (!cloudBackoff.ready(now)) return;
  const NetworkStatus status = mayapGetNetworkStatus();
  if (!(status.requestedMode == ConnectivityMode::Online && status.connected)) return;
  lastHeartbeatAt = now;
  if (sendHeartbeat()) {
    cloudBackoff.onSuccess();
  } else {
    if (!requestDeferred) cloudBackoff.onFailure(millis());
  }
}

inline void serviceRegister(uint32_t now) {
  if (registered) return;
  if (!cloudBackoff.ready(now)) return;
  const NetworkStatus status = mayapGetNetworkStatus();
  if (status.requestedMode != ConnectivityMode::Online) return;
  if (!status.connected) {
    mayapSetProvisioningState(MayapProvisioningState::CloudOffline);
    return;
  }
  if (sendRegister()) {
    registered = true;
    cloudBackoff.onSuccess();
  } else {
    if (!requestDeferred) cloudBackoff.onFailure(millis());
  }
}

inline void servicePinReset() {
  if (!__atomic_exchange_n(&pinResetRequestFlag, 0U, __ATOMIC_ACQ_REL)) return;
  const NetworkStatus netStatus = mayapGetNetworkStatus();
  if (netStatus.requestedMode != ConnectivityMode::Online || !netStatus.connected) {
    mayapSerialPrintf(false, "[CLOUD] reset-pin bi huy: khong online luc yeu cau\n");
    return;
  }
  if (!sendResetPin() && requestDeferred) {
    // No HTTP request was sent: retain the user's intent while admission is
    // busy. Never automatically replay a reset after an ambiguous HTTP error.
    __atomic_store_n(&pinResetRequestFlag, 1U, __ATOMIC_RELEASE);
  }
}

}  // namespace MayapCloudInternal

// ================================ API cong khai ================================

inline void mayapCloudAlertBegin() {
  // Khong can khoi tao gi truoc: moi client HTTPS la ngan han, tao khi can goi.
}

// Called after an owner I/O operation has unwound its local HTTP/TLS session.
// Keep identity, PIN, event outbox and transaction data; retry registration
// through the existing backoff rather than wiping provisioning state.
inline void mayapCloudRecover(uint32_t now) {
  MayapCloudInternal::registered = false;
  MayapCloudInternal::cloudBackoff.onFailure(now);
}

// Goi tu controlTask (qua HmiCommandType::CloudPinReset, xem
// machine_control.h) khi nguoi lap dat xac nhan "Dat lai ma PIN" tren HMI.
// Chi dat co hieu cho networkTask - KHONG tu goi HTTPS o day (controlTask
// khong duoc phep block).
inline void mayapRequestCloudPinReset() {
  __atomic_store_n(&MayapCloudInternal::pinResetRequestFlag, 1U, __ATOMIC_RELEASE);
}

// Chi duoc goi tu networkTask (khong blocking task khac; ban than no CO the
// block chinh networkTask vai giay khi thuc su goi HTTPS, xem ghi chu dau file).
inline void mayapCloudAlertUpdate(uint32_t now) {
  using namespace MayapCloudInternal;

  if (!mayapDeviceSecret()[0] || !CLOUD_API_HOST[0]) {
    // Nguyen nhan PHO BIEN NHAT khien khong co canh bao nao duoc gui: worker
    // host/device_key la macro build-time trong config.h (MAYAP_CLOUD_API_HOST/
    // MAYAP_DEVICE_SECRET), chua duoc dat luc build. In canh bao ro rang, lap
    // lai dinh ky (khong lien tuc) de khong bi troi mat trong log nhung van
    // chac chan duoc nhin thay.
    static uint32_t lastConfigWarnAt = 0U;
    if (lastConfigWarnAt == 0U || MayapCloudInternal::timeReached(now, lastConfigWarnAt + 300000UL)) {
      lastConfigWarnAt = now;
      mayapSerialPrintf(false,
          "[CLOUD] CANH BAO: chua cau hinh MAYAP_CLOUD_API_HOST/MAYAP_DEVICE_SECRET "
          "trong firmware (config.h) - se KHONG gui duoc canh bao nao cho toi khi "
          "nguoi lap dat nap lai firmware voi cau hinh hop le.\n");
    }
    return;
  }

  static uint32_t lastCheckAt = 0U;
  // Goi ro namespace: bien ngoai "using namespace" dua ten nay vao ngang
  // hang voi timeReached() global cua hmi.h (khong bi che khuat nhu khi goi
  // tu BEN TRONG namespace), gay loi bien dich "goi ham mo ho" (ambiguous).
  if (MayapCloudInternal::timeReached(now, lastCheckAt + CLOUD_CHECK_INTERVAL_MS)) {
    lastCheckAt = now;
    portENTER_CRITICAL(&cloudMux);
    const bool valid = knownRuntimeValid;
    if (valid) processingRuntime = knownRuntime;
    const bool configValid = knownConfigValid;
    if (configValid) processingConfig = knownConfig;
    const bool remindersValid = knownRemindersValid;
    if (remindersValid) processingReminders = knownReminders;
    portEXIT_CRITICAL(&cloudMux);
    if (valid) {
      checkFaults(now);
      checkTransitions(now);
      if (configValid) {
        checkPowerRestored(now);
        checkLightAfterBatch(now);
        checkBatchSchedule(now);
        checkTurnCycleMissed(now);
      }
      if (remindersValid) checkCustomReminders(now);
    }
    checkWifiSignal(now);
  }

  // Explicit user request gets first admission, not a permanently occupied
  // send gap left by routine heartbeat/alarm traffic.
  servicePinReset();
  serviceRegister(now);
  if (registered) {
    serviceHeartbeat(now);
    drainOutbox(now);
  }

}

// MachineController goi ham nay tu controlTask, cung noi/cung nhip voi
// mayapWebSetRuntime() cua realtime_link.h (xem may_ap_industrial.ino/
// machine_control.h::copyRuntimeToHmi khu vuc goi hmiSetRuntime()).
inline void mayapCloudSetRuntime(const MachineRuntime &runtime) {
  using namespace MayapCloudInternal;
  portENTER_CRITICAL(&cloudMux);
  knownRuntime = runtime;
  knownRuntimeValid = true;
  portEXIT_CRITICAL(&cloudMux);
}

// Cung noi/cung nhip voi mayapWebSetConfig() cua realtime_link.h - chi can
// cho checkLightAfterBatch() biet lightAfterBatchAlarmEnabled dang BAT/TAT.
inline void mayapCloudSetConfig(const MachineConfig &config) {
  using namespace MayapCloudInternal;
  portENTER_CRITICAL(&cloudMux);
  knownConfig = config;
  knownConfigValid = true;
  portEXIT_CRITICAL(&cloudMux);
}

// Cung noi/cung nhip voi mayapWebSetReminders() cua realtime_link.h - cho
// checkCustomReminders() biet danh sach nhac nho hien co.
inline void mayapCloudSetReminders(const ReminderSet &reminders) {
  using namespace MayapCloudInternal;
  portENTER_CRITICAL(&cloudMux);
  knownReminders = reminders;
  knownRemindersValid = true;
  portEXIT_CRITICAL(&cloudMux);
}

// Trang thai SONG cua kenh Cloud Push (khac voi mayapPrintNetworkConfig() la
// cau hinh TINH) - dung cho lenh Serial CONFIG de debug day du: da dang ky
// voi Worker chua, hang doi con bao nhieu tin dang cho, backoff dang lui toi
// buoc may, lan gui/heartbeat gan nhat cach day bao lau. Goi ro namespace vi
// ham nay o pham vi global (xem ghi chu timeReached o mayapCloudAlertUpdate
// ben tren - cung ly do).
inline void mayapPrintCloudStatus(uint32_t now) {
  using namespace MayapCloudInternal;
  mayapSerialPrintf(false,
      "[CLOUD] host=%s device_key=%s da_dang_ky=%u outbox=%u/%u backoff_step=%u/%u\n",
      CLOUD_API_HOST[0] ? CLOUD_API_HOST : "(chua cau hinh)",
      mayapDeviceSecret()[0] ? "DA CAU HINH" : "CHUA CAU HINH",
      registered, static_cast<unsigned>(outboxCount), static_cast<unsigned>(CLOUD_OUTBOX_SIZE),
      static_cast<unsigned>(cloudBackoff.step), static_cast<unsigned>(BACKOFF_STEP_COUNT - 1U));
  const long sendAgoSec = lastSendAt == 0U
      ? -1L
      : static_cast<long>(MayapCloudInternal::elapsedMs(now, lastSendAt) / 1000U);
  const long heartbeatAgoSec = lastHeartbeatAt == 0U
      ? -1L
      : static_cast<long>(MayapCloudInternal::elapsedMs(now, lastHeartbeatAt) / 1000U);
  mayapSerialPrintf(false,
      "[CLOUD] lan_gui_gan_nhat=%lds_truoc lan_heartbeat_gan_nhat=%lds_truoc (-1 = chua tung)\n",
      sendAgoSec, heartbeatAgoSec);
}
