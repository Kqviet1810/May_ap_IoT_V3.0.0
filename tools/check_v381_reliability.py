from pathlib import Path
import re
import json

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise SystemExit(f"FAIL: {label}: missing {needle!r}")


def require_re(text: str, pattern: str, label: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise SystemExit(f"FAIL: {label}: pattern not found: {pattern}")


config = read("MAYAP_INDUSTRIAL_v4_0_0/config.h")
app = read("app.js")
identity = read("MAYAP_INDUSTRIAL_v4_0_0/device_identity.h")
cloud = read("MAYAP_INDUSTRIAL_v4_0_0/cloud_alert_link.h")
machine = read("MAYAP_INDUSTRIAL_v4_0_0/machine_control.h")
network = read("MAYAP_INDUSTRIAL_v4_0_0/network_service.h")
hmi = read("MAYAP_INDUSTRIAL_v4_0_0/hmi.h")
ota = read("MAYAP_INDUSTRIAL_v4_0_0/ota_update.h")
ino = read("MAYAP_INDUSTRIAL_v4_0_0/MAYAP_INDUSTRIAL_v4_0_0.ino")
wrangler = read("cloudflare/wrangler.toml")
wrapper = read("cloudflare/src/reliability-wrapper.js")
security = read("cloudflare/src/security-wrapper.js")
safety = read("doc/SAFETY_HARDWARE_REQUIREMENTS.md")
build_workflow = read(".github/workflows/build-firmware.yml")

release = json.loads(read("release-manifest.json"))["firmware"]
require_re(config, rf'MAYAP_FIRMWARE_VERSION\[\]\s*=\s*"{re.escape(release)}"', "firmware version")

# MQTT deploy image must fail at compile time if broker credentials are absent.
require(config, "static_assert(sizeof(MQTT_BROKER_HOST) > 1U", "MQTT host compile guard")
require(config, "static_assert(sizeof(MQTT_USERNAME) > 1U", "MQTT username compile guard")
require(config, "static_assert(sizeof(MQTT_PASSWORD) > 1U", "MQTT password compile guard")
require(build_workflow, 'username = "__ci_pr_mqtt_user__"', "PR MQTT compile placeholder")
require(build_workflow, 'password = "__ci_pr_mqtt_password__"', "PR MQTT password placeholder")
require(build_workflow, 'Thieu GitHub Secrets: MAYAP_MQTT_USERNAME/MAYAP_MQTT_PASSWORD', "deploy MQTT secrets gate")
require(build_workflow, "startsWith(github.ref, 'refs/heads/hardening/')", "hardening test artifact")
require(build_workflow, "firmware-test-${{ github.sha }}", "test artifact tied to commit SHA")

# Web MQTT session returned after page boot must update the live WEB object.
require(app, "WEB = Object.freeze({ ...WEB, ...runtimeMqtt });", "web MQTT runtime credential refresh")
require(app, "state.mqttSessionState = 'ready';", "web MQTT session ready state")
require(app, "if (mqttReady) connectMqtt();", "web MQTT init readiness gate")
require(app, "state.mqttSessionState === 'auth-required' && device", "explicit auth status without false device offline")
require(app, "state.mqttSessionState === 'error' && !state.mqtt", "visible broker/auth retry status")
require(app, "device.presenceEpoch === state.subscriptionEpoch && device.presence?.online === false", "device offline requires current presence")
require(app, "Date.now() - device.snapshotAt > WEB.staleAfterMs", "snapshot freshness independent of presence/config")


# Wi-Fi portal must quiesce cross-task network I/O before changing radio mode.
require(network, "PortalPhase::Quiescing", "Wi-Fi portal quiescing phase")
require(network, "WiFi.disconnect(false, false)", "portal disconnect keeps radio alive")
require(network, "portalOtaQuiescedFlag", "portal/OTA quiesce handshake")
require(network, "portalPhase != PortalPhase::Quiescing", "cancel during quiesce must not touch radio")
require(network, "[PORTAL-PANIC] stage=", "portal panic RTC breadcrumb")
require(ota, "mayapOtaQuiesceForWifiPortal", "ArduinoOTA portal quiesce helper")
require(ota, "if (MayapOtaInternal::inProgress)", "do not abort active ArduinoOTA")
require(ino, "mayapWifiPortalExclusiveRequested()", "otaTask portal exclusion")
require(ino, "mayapSetWifiPortalOtaQuiesced(quiesced)", "otaTask quiesce acknowledgement")
require(config, "WIFI_PORTAL_MAX_OPEN_MS = 120000UL", "Wi-Fi portal 2 minute network timeout")
require(config, "WIFI_PORTAL_UI_IDLE_TIMEOUT_MS = 120000UL", "Wi-Fi portal 2 minute HMI timeout")

# ATtiny v4: preserve power-loss protection with pulse-width bus framing.
attiny = read("ATTINY13A_POWER_ALARM/ATTINY13A_POWER_ALARM.ino")
attiny_bus = read("MAYAP_INDUSTRIAL_v4_0_0/attiny_bus.h")
attiny_doc = read("doc/attiny_power_alarm.md")
require(config, "ATTINY_PROTOCOL_VERSION = 4U", "ATtiny protocol v4")
require(config, "ATTINY_MSG_ACTIVITY_ON = 6U", "ATtiny activity-on command")
require(config, "ATTINY_MSG_ACTIVITY_OFF = 7U", "ATtiny activity-off command")
require(config, "ATTINY_STATUS_FLAG_9V_LOW = 2U", "ATtiny 9V status retained")
require(config, "ATTINY_STATUS_FLAG_ACTIVITY = 8U", "ATtiny activity status flag")
require(config, "ATTINY_ACTIVITY_OFF_CONFIRM_MS = 30000UL", "ATtiny activity off debounce")
require(config, "ATTINY_STATUS_IDLE_INTERVAL_MS = 30000UL", "ATtiny bounded idle link check")
require(config, "ATTINY_STATUS_ARMED_INTERVAL_MS = 10000UL", "ATtiny bounded armed link check")
require(attiny_bus, 'logFailure("BUS_NOT_IDLE")', "ATtiny stuck-low initial timeout")
require(attiny_bus, 'logFailure("RETRY_BUS_NOT_IDLE")', "ATtiny stuck-low retry timeout")
require(machine, "!attinyLinkChecked_ && elapsedMs(now, bootAt_) >= 10000UL", "ATtiny startup link watchdog")
require(machine, "physicalOut.turnLeft || physicalOut.turnRight", "outside-batch turning power-loss arm")
require(machine, "physicalOut.circulationFan || physicalOut.ventFan || physicalOut.heatMaster", "outside-batch fan/heat-master power-loss arm")
if "physicalOut.heaterSsr" in machine[machine.find("void updateAttinyLink"):machine.find("void updateBatchTime")]:
    raise SystemExit("FAIL: ATtiny activity must use heatMaster, not heaterSsr")
if "ATTINY_ACTIVITY_REASSERT_MS" in config or "ATTINY_ACTIVITY_REASSERT_MS" in machine:
    raise SystemExit("FAIL: 5-second ATtiny activity reassert reintroduced")
require(machine, "attinyActivitySynced_", "ATtiny two-way activity sync")
require(machine, "attinyBatchSync_.fault() || attinyActivitySync_.fault()", "ATtiny per-state E503 tracking")
require(machine, "!mayapAttinyBusCommandPending(oppositeBatchCommand)", "ATtiny stale queued batch command guard")
require(read("MAYAP_INDUSTRIAL_v4_0_0/attiny_state_sync.h"), "TRANSITION_TIMEOUT_MS = 8000UL", "ATtiny bounded state transition")
require(machine, "keepPowerLossArmed", "batch-stop no-gap handoff")
require(machine, "refreshPendingResumeElapsedFromRtc();", "pending resume elapsed follows RTC")
require(machine, "savedElapsedAtCheckpoint_) + delta", "pending resume elapsed uses checkpoint anchor")
require(attiny, "if (p & V9PIN)", "ATtiny 9V sensing retained")
require(attiny, "F_9VFAULT", "ATtiny 9V low reporting retained")
require(attiny, "flags & (F_BATCH | F_ACTIVITY)", "ATtiny power-loss alarm OR policy")
require(attiny, "F_PERSIST  (F_BATCH | F_ACTIVITY)", "ATtiny activity EEPROM persistence")
require(attiny, "F_ACTIVITY", "ATtiny activity status feedback")
require(attiny, "PRR |= _BV(PRADC) | _BV(PRTIM0)", "ATtiny ADC/timer power reduction")
require(attiny_bus, "RESPONSE_EDGES = 12U", "ATtiny v4 response edge capacity")
require(attiny_bus, "CAPTURE_EDGES = 14U", "ATtiny command and response edge capacity")
require(attiny_bus, "GPIO_MODE_INPUT_OUTPUT_OD", "ESP open-drain bus")
require(attiny_bus, "GPIO_PULLUP_ENABLE", "ESP 3.3V idle fallback pull-up")
require(attiny_bus, "esp_timer_start_once(txReleaseTimer_", "independent GPIO command timer")
require(attiny_bus, '"TX_PULSE_WIDTH"', "actual command pulse validation")
require(attiny_bus, "mayapEnsureCacheSafeGpioService()", "cache-safe GPIO capture")
require(attiny_bus, "now = millis()", "fresh bus clock after storage work")
require(attiny_doc, "Khong dung 470 kOhm", "ATtiny bus series resistor warning")
require(attiny_bus, "else if (bit != parity) return false", "ATtiny response parity")
require(attiny, "parity ^= one", "ATtiny transmit parity")
require(attiny, "PCMSK = BUS | PWRMASK", "ATtiny wake mask")
require(attiny, "sleepNow", "ATtiny race-free sleep helper")
require(attiny, "GIFR = _BV(PCIF)", "ATtiny clears stale pin-change")
require(attiny, "sei();\n  sleep_cpu();", "ATtiny atomic SEI/SLEEP sequence")
require(attiny, "DDRB  &= (uint8_t)~BUS", "ATtiny releases PB0 into Hi-Z")
require(attiny, "if (++q > 2120U) return 531U", "ATtiny stuck BUS measurement bound")
require(attiny, "GIMSK &= (uint8_t)~_BV(PCIE)", "ATtiny transaction freezes PCINT jitter")
if "_delay_ms(50)" in attiny:
    raise SystemExit("FAIL: ATtiny 50ms post-wake delay reintroduced")
require(attiny, "ESPPIN", "ATtiny digital 3V3 sense")
require(attiny, "V9PIN", "ATtiny digital 9V sense")
require(attiny_doc, "Den, coi va tao am khong arm rieng bao mat dien", "humidifier covered by batch arm")
require(attiny_bus, "if ((count != RESPONSE_EDGES && count != CAPTURE_EDGES) || overflow) return false", "ATtiny complete even-length frame guard")
require(attiny_bus, "if (busHigh() && count >= RESPONSE_EDGES", "ATtiny final LOW is never end-of-frame")
require(hmi, "(view == View::WifiChange) ? WIFI_PORTAL_UI_IDLE_TIMEOUT_MS", "Wi-Fi screen uses dedicated timeout")
require(network, "id=wifiPassword", "Wi-Fi portal password input id")
require(network, "id=showPassword", "Wi-Fi portal show-password control")
require(network, "document.getElementById", "Wi-Fi portal password visibility JS")
require(network, "this.checked?", "Wi-Fi portal password visibility toggle")

# Provisioning diagnostics must remain visible on the local HMI path.
for state in ["CloudOffline", "TlsError", "ServerDenied", "KeyMismatch", "CloudError"]:
    require(identity, f"MayapProvisioningState::{state}", f"provisioning state {state}")
require(identity, "mayapProvisioningStateText()", "provisioning state text")
require(identity, "mayapProvisioningStateText();", "HMI PIN fallback uses provisioning state")

# Cloud must classify provisioning failures rather than looping forever as 'syncing'.
require(cloud, "int *responseCode = nullptr", "HTTP response code propagation")
require(cloud, "MayapProvisioningState::ServerDenied", "HTTP 403 classification")
require(cloud, "MayapProvisioningState::KeyMismatch", "HTTP 401 classification")
require(cloud, "MayapProvisioningState::CloudOffline", "offline classification")

# Current-scale policy: auto provisioning is enabled, but inventory remains available.
require(wrangler, 'main = "src/account-worker.js"', "account worker entrypoint")
require(read("cloudflare/src/account-worker.js"), "physicalWorker.fetch(request,env,ctx)", "physical device reliability chain")
require(wrangler, 'REQUIRE_DEVICE_INVENTORY = "0"', "auto provisioning default")
require(wrangler, "MAX_NEW_DEVICE_REGISTRATIONS_PER_HOUR", "registration rate limit setting")
require(wrangler, "NEW_DEVICE_REGISTRATION_WINDOW_MINUTES", "registration window setting")
require(wrapper, "strictInventoryRequired", "optional strict inventory mode")
require(wrapper, "registrationWindowMs", "configurable registration window")
require(wrapper, "recordNewDeviceAdmission", "new-device rate accounting")
require(wrapper, "INSERT OR IGNORE INTO device_inventory", "auto-admit inventory record")
require(wrapper, "handleResetPin", "reset-pin recovery path")
require(wrapper, "provisioned: true", "reset-pin provisioning result")

# Do not weaken the original device-key gate: reliability-wrapper must feed through it.
require(security, "handleSecureRegister", "security register gate")
require(wrapper, "return worker.fetch(request, env, ctx);", "delegate to security wrapper")

# Safety consistency: SensorFrozen requires actual heater-on evidence and remains fail-safe.
require(machine, "heaterStuckAccumOnMs_ >= frozenEvidenceOnMs", "SensorFrozen heater evidence")
require_re(
    machine,
    r'\{FaultCode::SensorFrozen,\s*FaultSeverity::Stop,.*?true,\s*true,\s*true,\s*false,\s*true,\s*true,\s*"SENSOR FROZEN"\}',
    "SensorFrozen descriptor",
)
require(safety, "fault ở mức STOP và cắt cả SSR lẫn contactor nhiệt", "SensorFrozen safety documentation")

# Security regression tripwires.
firmware_text = "\n".join(
    p.read_text(encoding="utf-8", errors="ignore")
    for p in (ROOT / "MAYAP_INDUSTRIAL_v4_0_0").glob("*")
    if p.suffix in {".h", ".ino"}
)
# LOG/EXIT must silence every firmware-originated diagnostic. Keep the three
# explicit toggle confirmations, but never let a module bypass that gate.
firmware_sources = list((ROOT / "MAYAP_INDUSTRIAL_v4_0_0").glob("*.h")) + list(
    (ROOT / "MAYAP_INDUSTRIAL_v4_0_0").glob("*.ino")
)
for source in firmware_sources:
    source_text = source.read_text(encoding="utf-8", errors="ignore")
    direct_serial = re.findall(r"\bSerial\.(?:print|printf|write|flush)\s*\(", source_text)
    if source.name == "machine_control.h":
        if direct_serial:
            raise SystemExit("FAIL: controlTask must not write Serial directly")
        forced = re.findall(r'mayapSerialPrintf\(true,\s*"([^"\n]*)"', source_text)
        if forced != ["[SERIAL] %s\\n", "[SERIAL] ON (LOG)\\n", "[SERIAL] OFF (EXIT)\\n"]:
            raise SystemExit("FAIL: unsolicited forced Serial output")
    elif source.name == "serial_diagnostics.h":
        if direct_serial != ["Serial.write("] or "mayapSerialDebugEnabled()" not in source_text:
            raise SystemExit("FAIL: bounded Serial writer must preserve debug gate")
        require(source_text, "inline void mayapSerialDrain()", "sole bounded Serial writer")
        require(source_text, "criticalDropped", "Serial loss accounting")
    elif direct_serial or "mayapSerialPrintf(true" in source_text:
        raise SystemExit(f"FAIL: ungated Serial output in {source.name}")
require(config, "HEALTH_HEAP_SAMPLE_INTERVAL_MS = 1000UL", "bounded heap sampling")
require(machine, "serviceHealthHeap(now, healthHeapLastBest_", "E401 uses recovered heap")
if "setInsecure()" in firmware_text:
    raise SystemExit("FAIL: setInsecure() reintroduced")
if "BEGIN PRIVATE KEY" in firmware_text or "BEGIN EC PRIVATE KEY" in firmware_text:
    raise SystemExit("FAIL: private signing key found in firmware tree")

require(config, "PIN_OUT_HUMIDIFIER", "single-relay humidifier GPIO")
require(config, "humidifierEnabled = false", "humidifier optional default off")
require(machine, "config_.humidifierInstalled &&", "humidifier hardware gate")
require(machine, "config_.humidifierEnabled && batchRunning_", "humidifier batch permit")
require(machine, "config_.humidifierHysteresisRh", "humidifier hysteresis")
require(app, "'humidifierEnabled',", "web humidifier config field")
require(app, "outputHumidifierTile", "web humidifier feature-gated runtime tile")
require(app, "syncHumidifierFeatureUi", "web humidifier feature gate")
require(config, "ventScheduleEnabled = false", "periodic ventilation default off")
require(machine, "scheduledVentActive", "periodic ventilation RTC control")
require(machine, "CONFIG_SCHEMA = 12", "config schema 12 for ventilation profile")
require(machine, "CONFIG_SCHEMA_LEGACY_V11 = 11", "schema 11 migration")
require(machine, "ventProfileDutyPercent", "day-based ventilation profile")
require(machine, "ventFanForceOn", "thermal ventilation override")
require(app, "ventDutyDay19To21", "web ventilation profile")

print(f"v{release} reliability regression checks: OK")
