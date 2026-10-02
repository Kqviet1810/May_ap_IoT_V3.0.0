from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def load(rel):
    p = ROOT / rel
    return p, p.read_text(encoding="utf-8")


def save(p, text):
    p.write_text(text, encoding="utf-8")


def replace_once(text, old, new, label):
    n = text.count(old)
    if n != 1:
        raise SystemExit(f"{label}: expected exactly 1 match, got {n}")
    return text.replace(old, new, 1)


def regex_once(text, pattern, repl, label, flags=re.S):
    out, n = re.subn(pattern, repl, text, count=1, flags=flags)
    if n != 1:
        raise SystemExit(f"{label}: expected exactly 1 regex match, got {n}")
    return out

# -----------------------------------------------------------------------------
# config.h: dedicated test-heater timing + HMI supervision thresholds.
# -----------------------------------------------------------------------------
p, s = load("MAYAP_INDUSTRIAL_v3_4_0/config.h")
s = replace_once(
    s,
    "constexpr uint32_t HMI_HEARTBEAT_TIMEOUT_MS = 2000UL;\n"
    "constexpr uint32_t SUPERVISOR_RESTART_FALLBACK_MS = 7000UL; // TWDT 5 s duoc uu tien; day la fallback\n",
    "constexpr uint32_t HMI_HEARTBEAT_TIMEOUT_MS = 2000UL;\n"
    "// HMI cham thoang qua chi canh bao; neu treo that su lau hon 8 s hoac\n"
    "// lien tuc co chu ky >1.2 s thi supervisor khoi dong lai CO KIEM SOAT:\n"
    "// latch output an toan -> suspend control/HMI -> safe outputs -> restart.\n"
    "constexpr uint32_t HMI_FATAL_HEARTBEAT_TIMEOUT_MS = 8000UL;\n"
    "constexpr uint32_t HMI_CYCLE_TRIP_US = 1200000UL;\n"
    "constexpr uint8_t HMI_CYCLE_TRIP_COUNT = 3U;\n"
    "constexpr uint32_t SUPERVISOR_RESTART_FALLBACK_MS = 7000UL; // TWDT 5 s duoc uu tien; day la fallback\n",
    "config HMI supervision",
)
s = replace_once(
    s,
    "constexpr uint32_t TEST_OUTPUT_HOLD_MAX_MS = 20000UL;\n"
    "constexpr uint32_t TEST_LIMIT_TIMEOUT_MS = 20000UL;\n",
    "constexpr uint32_t TEST_OUTPUT_HOLD_MAX_MS = 20000UL;\n"
    "// Test SSR nhiet la quy trinh rieng: quat tuan hoan chay truoc 5 s,\n"
    "// sau do thanh nhiet duoc phep ON toi da 3 phut. Day la tran CUNG;\n"
    "// nut HMI co the dung som hon nhung van phai qua 2 man xac nhan.\n"
    "constexpr uint32_t TEST_HEATER_FAN_PRESTART_MS = 5000UL;\n"
    "constexpr uint32_t TEST_HEATER_HOLD_MAX_MS = 180000UL;\n"
    "// Quat hut trong buoc xac nhan sau test nhiet: cho nguoi lap dat du\n"
    "// thoi gian quan sat, nhung khong the bi bo quen ON vo han.\n"
    "constexpr uint32_t TEST_VENT_CONFIRM_HOLD_MAX_MS = 60000UL;\n"
    "constexpr uint32_t TEST_LIMIT_TIMEOUT_MS = 20000UL;\n",
    "config test timing",
)
save(p, s)

# -----------------------------------------------------------------------------
# machine_control.h: thermal response watchdog, frozen-sensor hard stop,
# protected config while batch/resume, and safer commissioning test mode.
# -----------------------------------------------------------------------------
p, s = load("MAYAP_INDUSTRIAL_v3_4_0/machine_control.h")

s = replace_once(
    s,
    '{FaultCode::SensorFrozen, FaultSeverity::Warning, 58U, AlarmSensor, false, false, false, false, false, false, "SENSOR FROZEN"},',
    '{FaultCode::SensorFrozen, FaultSeverity::Stop, 228U, AlarmSensor, true, true, true, false, true, true, "SENSOR FROZEN"},',
    "sensor frozen descriptor",
)
s = replace_once(
    s,
    '{FaultCode::HeaterNotHeating, FaultSeverity::Warning, 65U, AlarmSystem, false, false, false, false, false, false, "HEATER NOT HEATING"},',
    '{FaultCode::HeaterNotHeating, FaultSeverity::Stop, 226U, AlarmSystem, true, true, true, false, true, true, "HEATER NOT HEATING"},',
    "heater response descriptor",
)

# Replace old continuous-SSR detector with equivalent-full-power ON-time watchdog.
pattern = r'''    // Thanh nhiet duoc lenh BAT lien tuc \(khong ngat quang\) qua\n.*?    const bool sensorGrace = !timeReached\(now, sensorStartupGraceUntil_\) &&'''
repl = '''    // Watchdog dap ung nhiet: khong con doi SSR phai ON LIEN TUC. PID co the\n    // chia xung theo cua so nen ta tich luy thoi gian SSR THUC SU ON (tuong\n    // duong nang luong full-power). Neu dang can gia nhiet ma da cap du tong\n    // on-time cau hinh nhung PV khong tang toi thieu -> nghi thanh nhiet/SSR/\n    // sensor khong dap ung. Loi nay cat CA SSR + contactor tong va bat 2 quat.\n    const bool responseDemand = batchRunning_ && inputs_.state().heaterEnable &&\n        sensorUsable_ && isfinite(temperature_) &&\n        temperature_ < config_.targetTemp - config_.tempHysteresis;\n    const uint32_t responseRequiredOnMs =\n        static_cast<uint32_t>(config_.heaterStuckDurationSec) * 1000UL;\n    if (!responseDemand) {\n      heaterStuckTracking_ = false;\n      heaterStuckSinceAt_ = now;\n      heaterStuckAccumOnMs_ = 0U;\n      heaterStuckStartTemp_ = NAN;\n      heaterNotHeatingActive_ = false;\n    } else {\n      if (!heaterStuckTracking_) {\n        heaterStuckTracking_ = true;\n        heaterStuckSinceAt_ = now;\n        heaterStuckAccumOnMs_ = 0U;\n        heaterStuckStartTemp_ = temperature_;\n        heaterNotHeatingActive_ = false;\n      } else {\n        // Gioi han dt 1 s de mot lan task tre bat thuong khong duoc tinh nhu\n        // da cap nhiet lien tuc trong ca khoang tre do.\n        const uint32_t dt = std::min<uint32_t>(elapsedMs(now, heaterStuckSinceAt_), 1000UL);\n        heaterStuckSinceAt_ = now;\n        if (outputs_.state().heaterSsr &&\n            heaterStuckAccumOnMs_ < UINT32_MAX - dt) {\n          heaterStuckAccumOnMs_ += dt;\n        }\n        const float rise = isfinite(heaterStuckStartTemp_)\n            ? temperature_ - heaterStuckStartTemp_ : 0.0f;\n        if (isfinite(heaterStuckStartTemp_) &&\n            rise >= config_.heaterStuckMinRiseC) {\n          // He thong co dap ung: bat dau cua so nang luong moi tu PV hien tai.\n          heaterStuckStartTemp_ = temperature_;\n          heaterStuckAccumOnMs_ = 0U;\n          heaterNotHeatingActive_ = false;\n        } else if (heaterStuckAccumOnMs_ >= responseRequiredOnMs) {\n          heaterNotHeatingActive_ = true;\n        }\n      }\n    }\n\n    const bool sensorGrace = !timeReached(now, sensorStartupGraceUntil_) &&'''
s = regex_once(s, pattern, repl, "thermal response watchdog")

old = '''    const bool sensorFrozenActive = batchRunning_ && sensorUsable_ &&
        isfinite(sensorFrozenRefTemp_) &&
        elapsedMs(now, sensorFrozenSince_) >= SENSOR_FROZEN_TIMEOUT_MS;'''
new = '''    // Khong cat nhiet chi vi buong dang on dinh that su. SensorFrozen chi
    // duoc nang thanh STOP khi gia tri dung hinh lau VA trong chinh thoi gian
    // do heater da cap mot luong on-time dang ke ma PV van khong nhuc nhich.
    // Dieu nay tranh false-trip o diem dat, nhung van bat duoc tinh huong nguy
    // hiem "sensor ket o muc thap -> PID cu tiep tuc gia nhiet".
    const uint32_t frozenEvidenceOnMs = std::max<uint32_t>(60000UL,
        (static_cast<uint32_t>(config_.heaterStuckDurationSec) * 1000UL) / 2UL);
    const bool sensorFrozenActive = batchRunning_ && sensorUsable_ &&
        isfinite(sensorFrozenRefTemp_) &&
        elapsedMs(now, sensorFrozenSince_) >= SENSOR_FROZEN_TIMEOUT_MS &&
        heaterStuckAccumOnMs_ >= frozenEvidenceOnMs;'''
s = replace_once(s, old, new, "sensor frozen gate")

s = replace_once(
    s,
    '''  bool heaterStuckTracking_ = false;
  uint32_t heaterStuckSinceAt_ = 0U;
  float heaterStuckStartTemp_ = NAN;
  bool heaterNotHeatingActive_ = false;''',
    '''  bool heaterStuckTracking_ = false;
  uint32_t heaterStuckSinceAt_ = 0U;
  uint32_t heaterStuckAccumOnMs_ = 0U;
  float heaterStuckStartTemp_ = NAN;
  bool heaterNotHeatingActive_ = false;''',
    "heater response fields",
)

# Protect commissioning/safety/tuning settings during an active or pending batch.
pattern = r'''      // xem settingLockedDuringBatch\(\) trong hmi\.h va man xac nhan CO/HUY\n.*?      const bool protectedBatchChange = \(batchRunning_ \|\| resumePending_\) &&\n          \(requested\.totalIncubationDays != config_\.totalIncubationDays \|\|\n           \(requested\.connectivityMode == ConnectivityMode::Online &&\n            config_\.connectivityMode != ConnectivityMode::Online\)\);'''
repl = '''      // Trong me/resume chi mo cac tham so VAN HANH: SV, bao am, nguong
      // quat hut, chu ky/bat-tat dao, dao tay dong lich va cho phep ONLINE->
      // OFFLINE. Calibration, nguong safety, PID/tuning, timeout co khi,
      // sensor/recovery va OFFLINE->ONLINE deu bi khoa o lop luu that su nay
      // de web/Serial/HMI khong the di vong qua khoa giao dien.
      const bool protectedBatchChange = (batchRunning_ || resumePending_) && (
          requested.autoResumeOnPowerLoss != config_.autoResumeOnPowerLoss ||
          requested.totalIncubationDays != config_.totalIncubationDays ||
          requested.lowTempAlarm != config_.lowTempAlarm ||
          requested.highTempAlarm != config_.highTempAlarm ||
          requested.emergencyTemp != config_.emergencyTemp ||
          requested.tempOffset != config_.tempOffset ||
          requested.humidityOffset != config_.humidityOffset ||
          requested.highTempAlarmWithoutBatch != config_.highTempAlarmWithoutBatch ||
          requested.kp != config_.kp || requested.ki != config_.ki ||
          requested.kd != config_.kd ||
          requested.pidCycleSec != config_.pidCycleSec ||
          requested.maxHeaterPower != config_.maxHeaterPower ||
          requested.heaterStuckMinRiseC != config_.heaterStuckMinRiseC ||
          requested.heaterStuckDurationSec != config_.heaterStuckDurationSec ||
          requested.tempRateLimitC != config_.tempRateLimitC ||
          requested.tempRateWindowSec != config_.tempRateWindowSec ||
          requested.tempOscillationCrossLimit != config_.tempOscillationCrossLimit ||
          requested.tempOscillationWindowSec != config_.tempOscillationWindowSec ||
          requested.autotuneRelayPowerPercent != config_.autotuneRelayPowerPercent ||
          requested.autotuneBandC != config_.autotuneBandC ||
          requested.turnMaxRunSec != config_.turnMaxRunSec ||
          requested.powerRestoreDelaySec != config_.powerRestoreDelaySec ||
          requested.sensorTimeoutSec != config_.sensorTimeoutSec ||
          requested.allowHeatWithoutBatch != config_.allowHeatWithoutBatch ||
          requested.sirenSelfTestEnabled != config_.sirenSelfTestEnabled ||
          (requested.connectivityMode == ConnectivityMode::Online &&
           config_.connectivityMode != ConnectivityMode::Online));'''
s = regex_once(s, pattern, repl, "protected config during batch")

# Test-output command: reject unsafe direction, dedicated heater/vent durations.
pattern = r'''  bool testOutputPulse\(uint32_t now, TestOutputId id, const char \*&message\) \{.*?\n  void testOutputStop\(uint32_t now, TestOutputId id\) \{.*?\n  \}\n\n  bool testLimitStart'''
repl = '''  bool testOutputPulse(uint32_t now, TestOutputId id, const char *&message) {
    if (!testModeActive_) { message = "CHUA VAO CHE DO TEST"; return false; }
    const uint8_t idx = static_cast<uint8_t>(id);
    if (idx >= static_cast<uint8_t>(TestOutputId::Count)) {
      message = "THIET BI KHONG HOP LE"; return false;
    }
    const InputState &in = inputs_.state();
    if ((id == TestOutputId::TurnLeft || id == TestOutputId::TurnRight) &&
        in.limitLeft && in.limitRight) {
      message = "HAI HANH TRINH CUNG ON"; return false;
    }
    if (id == TestOutputId::TurnLeft && in.limitLeft) {
      message = "DA O HANH TRINH TRAI"; return false;
    }
    if (id == TestOutputId::TurnRight && in.limitRight) {
      message = "DA O HANH TRINH PHAI"; return false;
    }
    if (id == TestOutputId::HeaterSsr) {
      const bool heaterSafe = sensorUsable_ && !faults_.masterDropRequired() &&
          !faults_.ssrInhibited() && !emergencyActive_ && !highTemperatureActive_;
      if (!heaterSafe) { message = "NHIET CHUA AN TOAN DE TEST"; return false; }
      testHeaterStartedAt_ = now;
      testHeaterPostCoolUntil_ = 0U;
      testOutputPulseUntil_[idx] = now + TEST_HEATER_FAN_PRESTART_MS +
                                   TEST_HEATER_HOLD_MAX_MS;
      testModeLastActivityAt_ = now;
      message = "QUAT CHAY TRUOC - CHO 5 GIAY";
      return true;
    }
    // Dao trai/phai loai tru nhau; OutputArbiter van giu dead-time 500 ms.
    if (id == TestOutputId::TurnLeft || id == TestOutputId::TurnRight) {
      const TestOutputId opposite = id == TestOutputId::TurnLeft
          ? TestOutputId::TurnRight : TestOutputId::TurnLeft;
      testOutputPulseUntil_[static_cast<uint8_t>(opposite)] = 0U;
      testTurnStartedAt_ = now;
      if (in.limitLeft) testTurnOrigin_ = TrayPosition::Left;
      else if (in.limitRight) testTurnOrigin_ = TrayPosition::Right;
      else testTurnOrigin_ = TrayPosition::Unknown;
      testTurnDirection_ = id;
    }
    const uint32_t hold = id == TestOutputId::VentFan
        ? TEST_VENT_CONFIRM_HOLD_MAX_MS : TEST_OUTPUT_HOLD_MAX_MS;
    testOutputPulseUntil_[idx] = now + hold;
    testModeLastActivityAt_ = now;
    message = "DANG BAT THIET BI";
    return true;
  }

  void testOutputStop(uint32_t now, TestOutputId id) {
    const uint8_t idx = static_cast<uint8_t>(id);
    if (idx >= static_cast<uint8_t>(TestOutputId::Count)) return;
    testOutputPulseUntil_[idx] = 0U;
    if (id == TestOutputId::HeaterSsr) {
      testHeaterStartedAt_ = 0U;
      testHeaterPostCoolUntil_ = now + POST_COOL_MS;
    }
    if (id == TestOutputId::TurnLeft || id == TestOutputId::TurnRight) {
      testTurnStartedAt_ = 0U;
      testTurnOrigin_ = TrayPosition::Unknown;
      testTurnDirection_ = TestOutputId::Count;
    }
    testModeLastActivityAt_ = now;
  }

  bool testLimitStart'''
s = regex_once(s, pattern, repl, "test output commands")

# Reset new test state on enter/exit.
s = replace_once(
    s,
    '''    testOutputMaskActive_ = 0U;
    for (uint32_t &t : testOutputPulseUntil_) t = 0U;
    testLimitPhase_ = TestLimitPhase::Idle;''',
    '''    testOutputMaskActive_ = 0U;
    for (uint32_t &t : testOutputPulseUntil_) t = 0U;
    testHeaterStartedAt_ = 0U;
    testHeaterPostCoolUntil_ = 0U;
    testTurnStartedAt_ = 0U;
    testTurnOrigin_ = TrayPosition::Unknown;
    testTurnDirection_ = TestOutputId::Count;
    testLimitPhase_ = TestLimitPhase::Idle;''',
    "test enter reset state",
)
s = replace_once(
    s,
    '''    testModeActive_ = false;
    testOutputMaskActive_ = 0U;
    for (uint32_t &t : testOutputPulseUntil_) t = 0U;
    testLimitPhase_ = TestLimitPhase::Idle;''',
    '''    testModeActive_ = false;
    testOutputMaskActive_ = 0U;
    for (uint32_t &t : testOutputPulseUntil_) t = 0U;
    testHeaterStartedAt_ = 0U;
    testHeaterPostCoolUntil_ = 0U;
    testTurnStartedAt_ = 0U;
    testTurnOrigin_ = TrayPosition::Unknown;
    testTurnDirection_ = TestOutputId::Count;
    testLimitPhase_ = TestLimitPhase::Idle;''',
    "test exit reset state",
)

# Insert continuous test safety interlocks after limit-test state machine.
needle = '''    } else if (testLimitPhase_ == TestLimitPhase::Success &&
               timeReached(now, testLimitBuzzUntil_)) {
      testLimitPhase_ = TestLimitPhase::Idle;
    }
    // Quen thoat trang thu nghiem: tu dong ve trang thai an toan sau mot thoi'''
insert = '''    } else if (testLimitPhase_ == TestLimitPhase::Success &&
               timeReached(now, testLimitBuzzUntil_)) {
      testLimitPhase_ = TestLimitPhase::Idle;
    }

    // SSR test: het 3 phut nhiet that (sau 5 s prestart) hoac safety mat thi
    // cat heater ngay. Ghi lai moc activity de HMI con du thoi gian hoi 2
    // cau xac nhan, khong bi idle-timeout da ra khoi Test Mode ngay lap tuc.
    uint32_t &heaterDeadline = testOutputPulseUntil_[static_cast<uint8_t>(TestOutputId::HeaterSsr)];
    if (heaterDeadline != 0U) {
      const bool heaterSafe = sensorUsable_ && !faults_.masterDropRequired() &&
          !faults_.ssrInhibited() && !emergencyActive_ && !highTemperatureActive_;
      if (!heaterSafe || timeReached(now, heaterDeadline)) {
        heaterDeadline = 0U;
        testHeaterStartedAt_ = 0U;
        testHeaterPostCoolUntil_ = now + POST_COOL_MS;
        testModeLastActivityAt_ = now;
      }
    }

    // Test motor cung ton trong CTHT nhu van hanh that, khong con la cap relay
    // tho. Cham dich -> OFF ngay; hai CTHT cung ON / CTHT goc khong nha ->
    // OFF va latch loi co khi. OutputArbiter van dam bao dead-time doi chieu.
    const bool testLeft = testOutputPulseUntil_[static_cast<uint8_t>(TestOutputId::TurnLeft)] != 0U &&
        !timeReached(now, testOutputPulseUntil_[static_cast<uint8_t>(TestOutputId::TurnLeft)]);
    const bool testRight = testOutputPulseUntil_[static_cast<uint8_t>(TestOutputId::TurnRight)] != 0U &&
        !timeReached(now, testOutputPulseUntil_[static_cast<uint8_t>(TestOutputId::TurnRight)]);
    if (testLeft || testRight) {
      bool stopTestTurn = false;
      if (in.limitLeft && in.limitRight) {
        stopTestTurn = true;
        latchTurnFault(FaultCode::TurnLimitConflict, "TEST: HAI HANH TRINH CUNG ON");
      } else if ((testLeft && in.limitLeft) || (testRight && in.limitRight)) {
        stopTestTurn = true; // den dung CTHT dich: ket thuc binh thuong
      } else {
        const bool originStillActive =
            (testLeft && testTurnOrigin_ == TrayPosition::Right && in.limitRight) ||
            (testRight && testTurnOrigin_ == TrayPosition::Left && in.limitLeft);
        if (originStillActive && testTurnStartedAt_ != 0U &&
            elapsedMs(now, testTurnStartedAt_) >= TURN_LIMIT_RELEASE_TIMEOUT_MS) {
          stopTestTurn = true;
          latchTurnFault(FaultCode::TurnLimitStuck, "TEST: HANH TRINH KHONG NHA");
        }
      }
      if (stopTestTurn) {
        testOutputPulseUntil_[static_cast<uint8_t>(TestOutputId::TurnLeft)] = 0U;
        testOutputPulseUntil_[static_cast<uint8_t>(TestOutputId::TurnRight)] = 0U;
        testTurnStartedAt_ = 0U;
        testTurnOrigin_ = TrayPosition::Unknown;
        testTurnDirection_ = TestOutputId::Count;
        testModeLastActivityAt_ = now;
      }
    }
    // Quen thoat trang thu nghiem: tu dong ve trang thai an toan sau mot thoi'''
s = replace_once(s, needle, insert, "continuous test safety")

# Keep Test Mode alive while the long heater request itself is active.
s = replace_once(
    s,
    '''    if (elapsedMs(now, testModeLastActivityAt_) >= TEST_MODE_IDLE_EXIT_MS) {
      exitTestMode(now);
    }''',
    '''    const bool longHeaterTestActive = heaterDeadline != 0U && !timeReached(now, heaterDeadline);
    if (!longHeaterTestActive &&
        elapsedMs(now, testModeLastActivityAt_) >= TEST_MODE_IDLE_EXIT_MS) {
      exitTestMode(now);
    }''',
    "test idle timeout",
)

# Replace output generation for Test Mode.
pattern = r'''  void updateTestModeOutputs\(uint32_t now\) \{.*?\n  \}\n\n  // ----------------------------- Batch'''
repl = '''  void updateTestModeOutputs(uint32_t now) {
    const auto pulseOn = [&](TestOutputId id) {
      const uint32_t deadline = testOutputPulseUntil_[static_cast<uint8_t>(id)];
      return deadline != 0U && !timeReached(now, deadline);
    };
    const InputState &in = inputs_.state();
    const bool heaterTestSafe = sensorUsable_ && !faults_.masterDropRequired() &&
        !faults_.ssrInhibited() && !emergencyActive_ && !highTemperatureActive_;
    const bool heaterRequested = pulseOn(TestOutputId::HeaterSsr);
    const bool heaterPrestartDone = heaterRequested && testHeaterStartedAt_ != 0U &&
        elapsedMs(now, testHeaterStartedAt_) >= TEST_HEATER_FAN_PRESTART_MS;
    const bool heaterPostCool = testHeaterPostCoolUntil_ != 0U &&
        !timeReached(now, testHeaterPostCoolUntil_);

    OutputRequest req{};
    req.heaterSsr = heaterPrestartDone && heaterTestSafe;
    req.heatMaster = (req.heaterSsr || pulseOn(TestOutputId::HeatMaster)) && heaterTestSafe;
    // Test SSR luon ep quat tuan hoan tu TRUOC khi cap nhiet va giu them
    // POST_COOL_MS sau khi dung; fault safety van co quyen ep 2 quat rieng.
    req.circulationFan = pulseOn(TestOutputId::CirculationFan) || heaterRequested ||
                         heaterPostCool || faults_.circulationForced();
    req.ventFan = pulseOn(TestOutputId::VentFan) || faults_.ventForced();
    req.light = pulseOn(TestOutputId::Light);
    req.siren = pulseOn(TestOutputId::Siren);
    // CTHT la interlock cuoi cung ngay tai output request, ngoai logic stop/
    // latch o updateTestMode(): du 1 chu ky state chua kip clear cung khong
    // the tiep tuc day motor vao dung dau hanh trinh.
    req.turnLeft = pulseOn(TestOutputId::TurnLeft) && !in.limitLeft && !in.limitRight;
    req.turnRight = pulseOn(TestOutputId::TurnRight) && !in.limitRight && !in.limitLeft;
    req.immediateMasterDrop = !heaterTestSafe || faults_.masterDropRequired() || !sensorUsable_;

    outputs_.update(now, req);
    runtime_.heaterPower = req.heaterSsr ? 100.0f : 0.0f;

    uint8_t mask = 0U;
    for (uint8_t i = 0; i < static_cast<uint8_t>(TestOutputId::Count); ++i) {
      if (pulseOn(static_cast<TestOutputId>(i))) mask |= static_cast<uint8_t>(1U << i);
    }
    testOutputMaskActive_ = mask;
  }

  // ----------------------------- Batch'''
s = regex_once(s, pattern, repl, "test output generation")

s = replace_once(
    s,
    '''  bool testModeActive_ = false;
  uint8_t testOutputMaskActive_ = 0U;
  uint32_t testOutputPulseUntil_[static_cast<uint8_t>(TestOutputId::Count)]{};
  TestLimitId testLimitTarget_ = TestLimitId::Left;''',
    '''  bool testModeActive_ = false;
  uint8_t testOutputMaskActive_ = 0U;
  uint32_t testOutputPulseUntil_[static_cast<uint8_t>(TestOutputId::Count)]{};
  uint32_t testHeaterStartedAt_ = 0U;
  uint32_t testHeaterPostCoolUntil_ = 0U;
  uint32_t testTurnStartedAt_ = 0U;
  TrayPosition testTurnOrigin_ = TrayPosition::Unknown;
  TestOutputId testTurnDirection_ = TestOutputId::Count;
  TestLimitId testLimitTarget_ = TestLimitId::Left;''',
    "test state fields",
)
save(p, s)

# -----------------------------------------------------------------------------
# hmi.h: lock service parameters in batch/resume; special 3-minute heater
# commissioning flow with mandatory heat + vent confirmations.
# -----------------------------------------------------------------------------
p, s = load("MAYAP_INDUSTRIAL_v3_4_0/hmi.h")

s = replace_once(
    s,
    '''bool settingLockedDuringBatch(uint8_t settingIndex) {
  if (!currentRuntime.batchRunning) return false;
  // Chi con khoa so ngay ap tong (thay doi giua chung se lam sai lich/ngay
  // du kien no). Cac tham so dao (bat/tat dao tu dong, chu ky, thoi gian
  // hanh trinh) KHONG con bi khoa khi dang ap nua - cho phep chinh nhu binh
  // thuong, rieng bat/tat dao tu dong se hoi CO/HUY truoc khi ap dung (xem
  // openTurningToggleConfirm()) vi day la thay doi anh huong truc tiep den
  // dao trung dang chay. So sanh theo offset field (khong phai chi so cung
  // trong mang SETTINGS[]) de khong vo tinh khoa nham muc khac neu sau nay
  // them/xoa/doi cho thong so trong bang (da tung la loi thuc te khi them
  // "Bu nhiet do").
  const uint16_t offset = SETTINGS[settingIndex].offset;
  return offset == offsetof(MachineConfig, totalIncubationDays);
}''',
    '''bool settingLockedDuringBatch(uint8_t settingIndex) {
  if (!currentRuntime.batchRunning && !currentRuntime.resumePending) return false;
  const uint16_t offset = SETTINGS[settingIndex].offset;
  // Mo dung nhom VAN HANH co the can chinh khi dang ap. Calibration, alarm
  // safety, PID/tuning, timeout co khi va recovery/system settings bi khoa.
  if (offset == offsetof(MachineConfig, targetTemp) ||
      offset == offsetof(MachineConfig, lowHumidityAlarm) ||
      offset == offsetof(MachineConfig, ventOnTemp) ||
      offset == offsetof(MachineConfig, ventOffTemp) ||
      offset == offsetof(MachineConfig, turningEnabled) ||
      offset == offsetof(MachineConfig, turnIntervalMin) ||
      offset == offsetof(MachineConfig, manualTurnReanchorsSchedule) ||
      offset == offsetof(MachineConfig, connectivityMode)) {
    return false;
  }
  return true;
}''',
    "HMI batch setting lock",
)

s = replace_once(
    s,
    '''enum class TestResult : uint8_t { Untested, Pass, Fail };
TestResult testDeviceResult[TEST_MODE_OUTPUT_ROWS] = {};
TestResult testLimitResult[TEST_MODE_LIMIT_ROWS] = {};
TestLimitPhase lastObservedTestLimitPhase = TestLimitPhase::Idle;
bool testDeviceConfirmActive = false;''',
    '''enum class TestResult : uint8_t { Untested, Pass, Fail };
TestResult testDeviceResult[TEST_MODE_OUTPUT_ROWS] = {};
TestResult testLimitResult[TEST_MODE_LIMIT_ROWS] = {};
TestLimitPhase lastObservedTestLimitPhase = TestLimitPhase::Idle;
// Workflow rieng cho SSR nhiet: 5s quat prestart -> toi da 3p gia nhiet ->
// hoi NHIET DA LEN? -> bat quat hut -> hoi QUAT HUT DA CHAY?.
enum class HeaterTestUiPhase : uint8_t { Idle, Heating, ConfirmHeat, ConfirmVent };
HeaterTestUiPhase heaterTestUiPhase = HeaterTestUiPhase::Idle;
uint32_t heaterTestUiStartedAt = 0U;
uint32_t heaterTestUiLastRefreshAt = 0U;
bool heaterTestConfirmYes = true;
bool testDeviceConfirmActive = false;''',
    "HMI heater workflow state",
)

# Add helper after queueCommand, before setListSelection.
needle = '''  if (commandId) *commandId = id;
  return true;
}

void setListSelection'''
insert = '''  if (commandId) *commandId = id;
  return true;
}

void finishHeaterPowerPhase(uint32_t now) {
  if (heaterTestUiPhase != HeaterTestUiPhase::Heating) return;
  (void)queueCommand(HmiCommandType::TestOutputStop, COMMAND_DEFAULT_VALID_MS,
                     0, static_cast<uint32_t>(TestOutputId::HeaterSsr));
  testModeLastCommandAt = now;
  heaterTestUiPhase = HeaterTestUiPhase::ConfirmHeat;
  heaterTestConfirmYes = true;
  armInputGuard();
  dirty = true;
}

void serviceHeaterTestWorkflow(uint32_t now) {
  if (heaterTestUiPhase == HeaterTestUiPhase::Idle) return;
  if (!currentRuntime.testModeActive) {
    heaterTestUiPhase = HeaterTestUiPhase::Idle;
    dirty = true;
    return;
  }
  if (heaterTestUiPhase == HeaterTestUiPhase::Heating) {
    const uint32_t totalMs = TEST_HEATER_FAN_PRESTART_MS + TEST_HEATER_HOLD_MAX_MS;
    const uint32_t elapsed = now - heaterTestUiStartedAt;
    const bool requestStillActive =
        (currentRuntime.testOutputMaskActive &
         (1U << static_cast<uint8_t>(TestOutputId::HeaterSsr))) != 0U;
    // 1.5 s dau cho command/control/runtime mailbox kip phan anh. Sau do neu
    // firmware da cat request vi safety thi cung chuyen sang buoc xac nhan,
    // khong de HMI dung o man "dang test" gia.
    if (elapsed >= totalMs || (elapsed >= 1500UL && !requestStillActive)) {
      finishHeaterPowerPhase(now);
      return;
    }
    if (now - heaterTestUiLastRefreshAt >= 500UL) {
      heaterTestUiLastRefreshAt = now;
      dirty = true;
    }
  }
}

void setListSelection'''
s = replace_once(s, needle, insert, "HMI heater workflow service")

# Long press must not bypass mandatory confirmations.
needle = '''  if (rotary.button == ButtonEvent::LongPress) {
    if (view == View::Home) {'''
insert = '''  if (rotary.button == ButtonEvent::LongPress) {
    // Trong workflow test nhiet, nhan giu KHONG duoc thoat tat ca va bo qua
    // 2 cau xac nhan bat buoc. Muon dung heater som thi nhan NGAN; o man
    // xac nhan phai chon CO/KHONG va nhan ngan de ghi ket qua.
    if (view == View::TestMode && heaterTestUiPhase != HeaterTestUiPhase::Idle) {
      resetRotaryPending();
      return;
    }
    if (view == View::Home) {'''
s = replace_once(s, needle, insert, "HMI long press heater guard")

# Replace TestMode input case with special workflow + generic fallback.
pattern = r'''    case View::TestMode: \{\n      // Sau khi xung mot thiet bi, hoi nguoi lap dat CO/KHONG thay no chay -.*?      break;\n    \}\n\n    case View::TestSummary:'''
repl = '''    case View::TestMode: {
      if (heaterTestUiPhase == HeaterTestUiPhase::Heating) {
        if (rotary.button == ButtonEvent::ShortPress) finishHeaterPowerPhase(now);
        break;
      }
      if (heaterTestUiPhase == HeaterTestUiPhase::ConfirmHeat) {
        if (rotary.step) { heaterTestConfirmYes = !heaterTestConfirmYes; dirty = true; }
        if (rotary.button == ButtonEvent::ShortPress) {
          testDeviceResult[static_cast<uint8_t>(TestOutputId::HeaterSsr)] =
              heaterTestConfirmYes ? TestResult::Pass : TestResult::Fail;
          if (queueCommand(HmiCommandType::TestOutputPulse, COMMAND_DEFAULT_VALID_MS,
                           0, static_cast<uint32_t>(TestOutputId::VentFan))) {
            testModeLastCommandAt = now;
            heaterTestUiPhase = HeaterTestUiPhase::ConfirmVent;
            heaterTestConfirmYes = true;
            armInputGuard();
          } else {
            testDeviceResult[static_cast<uint8_t>(TestOutputId::VentFan)] = TestResult::Fail;
            heaterTestUiPhase = HeaterTestUiPhase::Idle;
          }
          dirty = true;
        }
        break;
      }
      if (heaterTestUiPhase == HeaterTestUiPhase::ConfirmVent) {
        if (rotary.step) { heaterTestConfirmYes = !heaterTestConfirmYes; dirty = true; }
        if (rotary.button == ButtonEvent::ShortPress) {
          testDeviceResult[static_cast<uint8_t>(TestOutputId::VentFan)] =
              heaterTestConfirmYes ? TestResult::Pass : TestResult::Fail;
          (void)queueCommand(HmiCommandType::TestOutputStop, COMMAND_DEFAULT_VALID_MS,
                             0, static_cast<uint32_t>(TestOutputId::VentFan));
          testModeLastCommandAt = now;
          heaterTestUiPhase = HeaterTestUiPhase::Idle;
          dirty = true;
        }
        break;
      }

      // Cac output con lai van dung hoi CO/KHONG chung nhu cu.
      if (testDeviceConfirmActive) {
        if (rotary.step) {
          testDeviceConfirmYes = !testDeviceConfirmYes;
          dirty = true;
        }
        if (rotary.button == ButtonEvent::ShortPress) {
          testDeviceResult[testDeviceConfirmIndex] =
              testDeviceConfirmYes ? TestResult::Pass : TestResult::Fail;
          queueCommand(HmiCommandType::TestOutputStop, COMMAND_DEFAULT_VALID_MS,
                      0, static_cast<uint32_t>(testDeviceConfirmIndex));
          testModeLastCommandAt = now;
          testDeviceConfirmActive = false;
          dirty = true;
        }
        break;
      }
      if (rotary.step) {
        setListSelection(static_cast<int>(listIndex) + rotary.step,
                         TEST_MODE_ITEM_COUNT);
      }
      if (rotary.button == ButtonEvent::ShortPress) {
        if (listIndex < TEST_MODE_OUTPUT_ROWS) {
          if (queueCommand(HmiCommandType::TestOutputPulse,
                           COMMAND_DEFAULT_VALID_MS, 0,
                           static_cast<uint32_t>(listIndex))) {
            testModeLastCommandAt = now;
            if (listIndex == static_cast<uint8_t>(TestOutputId::HeaterSsr)) {
              heaterTestUiPhase = HeaterTestUiPhase::Heating;
              heaterTestUiStartedAt = now;
              heaterTestUiLastRefreshAt = now;
              heaterTestConfirmYes = true;
              testDeviceResult[static_cast<uint8_t>(TestOutputId::HeaterSsr)] = TestResult::Untested;
              testDeviceResult[static_cast<uint8_t>(TestOutputId::VentFan)] = TestResult::Untested;
            } else {
              testDeviceConfirmActive = true;
              testDeviceConfirmIndex = listIndex;
              testDeviceConfirmYes = true;
            }
            armInputGuard();
          }
        } else if (listIndex < TEST_MODE_OUTPUT_ROWS + TEST_MODE_LIMIT_ROWS) {
          testLimitSelected = (listIndex == TEST_MODE_OUTPUT_ROWS)
              ? TestLimitId::Left : TestLimitId::Right;
          queueCommand(HmiCommandType::TestLimitStart, COMMAND_DEFAULT_VALID_MS,
                      0, static_cast<uint32_t>(testLimitSelected));
          testModeLastCommandAt = now;
        } else {
          testSummaryIndex = 0;
          view = View::TestSummary;
        }
        dirty = true;
      }
      break;
    }

    case View::TestSummary:'''
s = regex_once(s, pattern, repl, "HMI TestMode input")

# Draw special heater workflow.
needle = '''void drawTestMode() {
  if (testDeviceConfirmActive) { drawTestDeviceConfirm(); return; }'''
insert = '''void drawHeaterTestWorkflow() {
  drawHeader("TEST NHIET", false);
  lcd.setFont(u8g2_font_6x12_tf);
  if (heaterTestUiPhase == HeaterTestUiPhase::Heating) {
    const uint32_t now = millis();
    const uint32_t elapsed = now - heaterTestUiStartedAt;
    char line[28];
    if (elapsed < TEST_HEATER_FAN_PRESTART_MS) {
      const uint32_t left = (TEST_HEATER_FAN_PRESTART_MS - elapsed + 999UL) / 1000UL;
      snprintf(line, sizeof(line), "QUAT KHOI DONG %lus", static_cast<unsigned long>(left));
      drawCenteredFit(24, line, u8g2_font_6x12_tf, u8g2_font_5x8_tf, u8g2_font_5x8_tf);
    } else {
      const uint32_t heatElapsed = elapsed - TEST_HEATER_FAN_PRESTART_MS;
      const uint32_t leftSec = heatElapsed >= TEST_HEATER_HOLD_MAX_MS ? 0U :
          (TEST_HEATER_HOLD_MAX_MS - heatElapsed + 999UL) / 1000UL;
      snprintf(line, sizeof(line), "CON LAI %02lu:%02lu",
               static_cast<unsigned long>(leftSec / 60UL),
               static_cast<unsigned long>(leftSec % 60UL));
      drawCenteredFit(24, line, u8g2_font_6x12_tf, u8g2_font_5x8_tf, u8g2_font_5x8_tf);
    }
    if (currentRuntime.sensorOnline) snprintf(line, sizeof(line), "NHIET %.1fC", currentRuntime.temperature);
    else snprintf(line, sizeof(line), "CAM BIEN LOI");
    drawCenteredFit(40, line, u8g2_font_helvB12_tf, u8g2_font_6x12_tf, u8g2_font_5x8_tf);
    drawCenteredFit(58, "NHAN = DUNG SOM", u8g2_font_5x8_tf,
                    u8g2_font_5x8_tf, u8g2_font_5x8_tf);
    return;
  }
  if (heaterTestUiPhase == HeaterTestUiPhase::ConfirmHeat) {
    drawCenteredFit(29, "NHIET DA LEN CHUA?", u8g2_font_6x12_tf,
                    u8g2_font_5x8_tf, u8g2_font_5x8_tf);
    drawYesNoButtons(heaterTestConfirmYes, "CO", "KHONG");
    return;
  }
  drawCenteredFit(29, "QUAT HUT DA CHAY?", u8g2_font_6x12_tf,
                  u8g2_font_5x8_tf, u8g2_font_5x8_tf);
  drawYesNoButtons(heaterTestConfirmYes, "CO", "KHONG");
}

void drawTestMode() {
  if (heaterTestUiPhase != HeaterTestUiPhase::Idle) { drawHeaterTestWorkflow(); return; }
  if (testDeviceConfirmActive) { drawTestDeviceConfirm(); return; }'''
s = replace_once(s, needle, insert, "HMI heater draw")

# Fault 104 should render as a real sensor fault instead of UNKNOWN.
s = replace_once(s, "    case 101: case 102: case 103: return AlarmSensor;",
                 "    case 101: case 102: case 103: case 104: return AlarmSensor;",
                 "HMI alarm map 104")
s = replace_once(s, '    case 103: return "CAM BIEN BAT THUONG";\n',
                 '    case 103: return "CAM BIEN BAT THUONG";\n    case 104: return "CAM BIEN DUNG HINH";\n',
                 "HMI fault title 104")
s = replace_once(s, '    case 103: snprintf(out, size, "MAU NGHI NGO %.1fC", fault.detail * 0.1f); break;\n',
                 '    case 103: snprintf(out, size, "MAU NGHI NGO %.1fC", fault.detail * 0.1f); break;\n    case 104: snprintf(out, size, "PV KET %.1fC - DA CAT NHIET", fault.detail * 0.1f); break;\n',
                 "HMI fault detail 104")

# Failed heater pulse must leave special screen instead of pretending it runs.
needle = '''    if (!ack.ok && command.type == HmiCommandType::TestModeEnter &&
        view == View::TestMode) {
      view = View::ChungMenu;'''
insert = '''    if (!ack.ok && command.type == HmiCommandType::TestOutputPulse &&
        command.alarmMask == static_cast<uint32_t>(TestOutputId::HeaterSsr)) {
      heaterTestUiPhase = HeaterTestUiPhase::Idle;
      dirty = true;
    }
    if (!ack.ok && command.type == HmiCommandType::TestModeEnter &&
        view == View::TestMode) {
      view = View::ChungMenu;'''
s = replace_once(s, needle, insert, "HMI rejected heater pulse")

# Periodic heater workflow service and no generic 60s menu timeout mid-workflow.
s = replace_once(
    s,
    '''void hmiUpdate(uint32_t now) {
  serviceApiMailboxes();
  stabilizeViewTransition();''',
    '''void hmiUpdate(uint32_t now) {
  serviceApiMailboxes();
  serviceHeaterTestWorkflow(now);
  stabilizeViewTransition();''',
    "HMI update heater workflow",
)
s = replace_once(
    s,
    '''  if (!confirmationActive() && view != View::Home && view != View::Alarm &&
      now - lastInteractionAt >= MENU_IDLE_TIMEOUT_MS) {''',
    '''  if (!confirmationActive() && heaterTestUiPhase == HeaterTestUiPhase::Idle &&
      view != View::Home && view != View::Alarm &&
      now - lastInteractionAt >= MENU_IDLE_TIMEOUT_MS) {''',
    "HMI menu timeout heater workflow",
)

save(p, s)

# -----------------------------------------------------------------------------
# .ino: HMI cycle instrumentation and controlled restart if HMI really hangs.
# -----------------------------------------------------------------------------
p, s = load("MAYAP_INDUSTRIAL_v3_4_0/MAYAP_INDUSTRIAL_v3_4_0.ino")
s = replace_once(
    s,
    '''static volatile uint32_t controlLastCycleUs = 0U;
static volatile uint32_t controlMaxCycleUs = 0U;
static volatile uint8_t controlTripCycleCount = 0U;''',
    '''static volatile uint32_t controlLastCycleUs = 0U;
static volatile uint32_t controlMaxCycleUs = 0U;
static volatile uint8_t controlTripCycleCount = 0U;
static volatile uint32_t hmiLastCycleUs = 0U;
static volatile uint32_t hmiMaxCycleUs = 0U;
static volatile uint8_t hmiTripCycleCount = 0U;''',
    "INO HMI metrics globals",
)

s = replace_once(
    s,
    '''void hmiTask(void *parameter) {
  (void)parameter;
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = millis();
    hmiUpdate(now);
    __atomic_store_n(&hmiHeartbeatMs, now, __ATOMIC_RELEASE);
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(HMI_TASK_PERIOD_MS));
  }
}''',
    '''void hmiTask(void *parameter) {
  (void)parameter;
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = millis();
    const int64_t cycleStartedUs = esp_timer_get_time();
    hmiUpdate(now);
    const uint32_t cycleUs = static_cast<uint32_t>(
        std::min<int64_t>(UINT32_MAX, esp_timer_get_time() - cycleStartedUs));
    __atomic_store_n(&hmiLastCycleUs, cycleUs, __ATOMIC_RELEASE);
    uint32_t previousMax = __atomic_load_n(&hmiMaxCycleUs, __ATOMIC_ACQUIRE);
    while (cycleUs > previousMax &&
           !__atomic_compare_exchange_n(&hmiMaxCycleUs, &previousMax, cycleUs,
                                        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
    uint8_t slowCount = __atomic_load_n(&hmiTripCycleCount, __ATOMIC_ACQUIRE);
    if (cycleUs >= HMI_CYCLE_TRIP_US) {
      if (slowCount < UINT8_MAX) ++slowCount;
    } else {
      slowCount = 0U;
    }
    __atomic_store_n(&hmiTripCycleCount, slowCount, __ATOMIC_RELEASE);
    // Ghi heartbeat SAU khi hmiUpdate tra ve va dung millis() moi nhat; neu
    // I2C/HMI bi block thi supervisor se thay stale dung thoi gian thuc.
    __atomic_store_n(&hmiHeartbeatMs, millis(), __ATOMIC_RELEASE);
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(HMI_TASK_PERIOD_MS));
  }
}''',
    "INO instrument HMI task",
)

needle = '''    const bool hmiHealthy = hmiBeat != 0U &&
        elapsedMs(now, hmiBeat) <= HMI_HEARTBEAT_TIMEOUT_MS;

    const uint8_t slowCycles = __atomic_load_n(
        &controlTripCycleCount, __ATOMIC_ACQUIRE);'''
insert = '''    const bool hmiHealthy = hmiBeat != 0U &&
        elapsedMs(now, hmiBeat) <= HMI_HEARTBEAT_TIMEOUT_MS;
    const uint8_t hmiSlowCycles = __atomic_load_n(&hmiTripCycleCount, __ATOMIC_ACQUIRE);
    const bool hmiFatal = hmiBeat != 0U &&
        (elapsedMs(now, hmiBeat) >= HMI_FATAL_HEARTBEAT_TIMEOUT_MS ||
         hmiSlowCycles >= HMI_CYCLE_TRIP_COUNT);

    const uint8_t slowCycles = __atomic_load_n(
        &controlTripCycleCount, __ATOMIC_ACQUIRE);'''
s = replace_once(s, needle, insert, "INO supervisor HMI fatal state")

needle = '''    if (hmiBeat != 0U && hmiHealthy != previousHmiHealthy) {
      previousHmiHealthy = hmiHealthy;
      mayapSerialPrintf(false, "[SUPERVISOR] HMI %s\\n",
                        hmiHealthy ? "RECOVERED" : "HEARTBEAT SLOW");
    }

    // Giam sat suc khoe he thong'''
insert = '''    if (hmiBeat != 0U && hmiHealthy != previousHmiHealthy) {
      previousHmiHealthy = hmiHealthy;
      mayapSerialPrintf(false, "[SUPERVISOR] HMI %s cycle=%luus slow=%u\\n",
                        hmiHealthy ? "RECOVERED" : "HEARTBEAT SLOW",
                        static_cast<unsigned long>(__atomic_load_n(&hmiLastCycleUs, __ATOMIC_ACQUIRE)),
                        static_cast<unsigned>(hmiSlowCycles));
    }

    if (hmiFatal) {
      // HMI chet that su khong duoc de may chay vo han ma nguoi van hanh mat
      // quyen quan sat/thao tac. Cat output an toan truoc, roi software reset;
      // neu dang co me, co che EEPROM/RTC hien co se phuc hoi theo policy reset.
      mayapLatchSystemTrip();
      if (controlTaskHandle) vTaskSuspend(controlTaskHandle);
      if (hmiTaskHandle) vTaskSuspend(hmiTaskHandle);
      mayapSafeOutputsEarly();
      mayapSerialPrintf(true,
          "[SUPERVISOR] HMI FATAL heartbeatAge=%lums cycle=%luus slow=%u -> RESTART\\n",
          static_cast<unsigned long>(elapsedMs(now, hmiBeat)),
          static_cast<unsigned long>(__atomic_load_n(&hmiLastCycleUs, __ATOMIC_ACQUIRE)),
          static_cast<unsigned>(hmiSlowCycles));
      esp_restart();
      abort();
    }

    // Giam sat suc khoe he thong'''
s = replace_once(s, needle, insert, "INO controlled HMI restart")

# Extend diagnostic stack/cycle line with HMI cycle max without changing cadence.
s = replace_once(
    s,
    '''          "[TASK] stack ctrl=%u hmi=%u sup=%u net=%u bytes cycle=%luus max=%luus\\n",
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(controlTaskHandle)),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(hmiTaskHandle)),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(supervisorTaskHandle)),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(networkTaskHandle)),
          static_cast<unsigned long>(__atomic_load_n(&controlLastCycleUs, __ATOMIC_ACQUIRE)),
          static_cast<unsigned long>(__atomic_load_n(&controlMaxCycleUs, __ATOMIC_ACQUIRE)));''',
    '''          "[TASK] stack ctrl=%u hmi=%u sup=%u net=%u bytes ctrl=%lu/%luus hmi=%lu/%luus\\n",
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(controlTaskHandle)),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(hmiTaskHandle)),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(supervisorTaskHandle)),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(networkTaskHandle)),
          static_cast<unsigned long>(__atomic_load_n(&controlLastCycleUs, __ATOMIC_ACQUIRE)),
          static_cast<unsigned long>(__atomic_load_n(&controlMaxCycleUs, __ATOMIC_ACQUIRE)),
          static_cast<unsigned long>(__atomic_load_n(&hmiLastCycleUs, __ATOMIC_ACQUIRE)),
          static_cast<unsigned long>(__atomic_load_n(&hmiMaxCycleUs, __ATOMIC_ACQUIRE)));''',
    "INO diagnostic HMI cycle",
)
save(p, s)

print("offline hardening patch applied successfully")
