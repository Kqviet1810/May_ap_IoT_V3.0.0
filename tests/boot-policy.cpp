#include "../MAYAP_INDUSTRIAL_v4_0_0/boot_policy.h"
#include <assert.h>
#include <stdio.h>
#include <initializer_list>
using namespace MayapBoot;

static void seal(Diagnostic &d) { d.checksum = checksum(d); }

int main() {
  Diagnostic empty{};
  Diagnostic d = beginDiagnostic(empty, 1U, ResetKind::PowerOn);
  assert(valid(d) && d.recoveryLevel == 0U && !d.bootCompleted);
  Diagnostic slot0 = d, slot1 = d;
  slot0.sequence = UINT32_MAX; seal(slot0);
  slot1.sequence = 0U; seal(slot1);
  assert(newestSlot(slot0, slot1) == 1U); // retained counter rollover
  slot1.checksum ^= 1U;
  assert(newestSlot(slot0, slot1) == 0U); // interrupted write keeps previous slot
  d.lastBootStage = Stage::Wifi;
  seal(d);
  for (uint32_t n = 1U; n <= 5U; ++n) {
    d = beginDiagnostic(d, 7U, ResetKind::Unexpected);
    assert(valid(d) && d.consecutiveFailedBoots == n);
    assert(d.recoveryLevel == (n < 3U ? n : 3U));
    if (n == 1U) assert(d.previousBootStage == Stage::Wifi && d.previousResetReason == 1U);
  }
  // Operator OTA/rollback is intentional; faults raised by firmware still count.
  for (RestartReason reason : {RestartReason::ArduinoOta, RestartReason::InternetOta, RestartReason::Rollback}) {
    d.plannedRestartReason = reason;
    strcpy(d.restartDetail, "operator action");
    seal(d);
    Diagnostic next = beginDiagnostic(d, 3U, ResetKind::Software);
    assert(next.consecutiveFailedBoots == d.consecutiveFailedBoots);
    assert(next.previousPlannedRestartReason == reason && next.plannedRestartReason == RestartReason::None);
    assert(!strcmp(next.previousRestartDetail, "operator action"));
    assert(next.bootCompleted == 0U);
  }
  for (ResetKind kind : {ResetKind::Unexpected, ResetKind::Brownout}) {
    Diagnostic next = beginDiagnostic(d, 9U, kind);
    assert(next.consecutiveFailedBoots == d.consecutiveFailedBoots + 1U);
    assert(next.previousPlannedRestartReason == d.plannedRestartReason); // breadcrumb, not an exemption
  }
  d.plannedRestartReason = RestartReason::HealthMonitor;
  d.bootCompleted = 1U;
  seal(d);
  Diagnostic next = beginDiagnostic(d, 3U, ResetKind::Software);
  assert(next.consecutiveFailedBoots == d.consecutiveFailedBoots + 1U);
  // Success is not a free pass for repeated runtime panic/WDT loops.
  next = beginDiagnostic(d, 7U, ResetKind::Unexpected);
  assert(next.consecutiveFailedBoots == d.consecutiveFailedBoots + 1U);
  next = beginDiagnostic(d, 1U, ResetKind::PowerOn);
  assert(next.consecutiveFailedBoots == 0U);
  d.checksum ^= 1U;
  assert(!valid(d));
  next = beginDiagnostic(d, 7U, ResetKind::Unexpected);
  assert(next.recoveryLevel == 0U);
  d = beginDiagnostic(empty, 1U, ResetKind::PowerOn);
  d.consecutiveFailedBoots = UINT32_MAX;
  seal(d);
  next = beginDiagnostic(d, 7U, ResetKind::Unexpected);
  assert(next.consecutiveFailedBoots == UINT32_MAX && next.recoveryLevel == 3U);

  Stability local;
  local.update(100U, true);
  assert(!local.held(25099U, SUCCESS_STABLE_MS));
  assert(local.held(25100U, SUCCESS_STABLE_MS));
  local.update(25101U, false);
  assert(!local.held(100000U, SUCCESS_STABLE_MS));
  local.update(30000U, true);
  assert(!local.held(54999U, SUCCESS_STABLE_MS));
  assert(local.held(55000U, SUCCESS_STABLE_MS));
  assert(!local.held(629999U, FAILURES_CLEAR_MS));
  assert(local.held(630000U, FAILURES_CLEAR_MS));
  local.update(UINT32_MAX - 100U, false);
  local.update(UINT32_MAX - 100U, true);
  assert(local.held(29899U, SUCCESS_STABLE_MS)); // millis rollover

  for (uint32_t level = 0U; level < 4U; ++level) {
    Sequencer flow;
    flow.begin(level, 0U);
    Stability healthy;
    healthy.update(1000U, true);
    while (flow.stage() != Stage::LocalSettle) flow.advance(1000U);
    assert(!flow.releaseNetwork(1000U + flow.networkDelay() - 1U, healthy));
    assert(flow.releaseNetwork(1000U + flow.networkDelay(), healthy));
    assert(flow.homeBeforeNetwork() == (level >= 2U));
    if (level == 3U) assert(flow.networkDelay() == 45000U);
    if (level == 1U) assert(flow.networkDelay() > LOCAL_SETTLE_MS && flow.serviceGap() > SERVICE_GAP_MS);
    healthy.update(45000U, false);
    assert(!flow.releaseNetwork(90000U, healthy));
    healthy.update(100000U, true);
    assert(flow.releaseNetwork(100000U + flow.networkDelay(), healthy));
    flow.advance(150000U);
    assert(flow.stage() == Stage::Wifi);
    assert(!flow.wifiDone(150000U, true, true));
    assert(!flow.wifiDone(160000U, false, false));
    assert(flow.wifiDone(150000U + flow.serviceGap(), true, false)); // offline/timeout
    flow.advance(160000U);
    assert(flow.stage() == Stage::Mqtt);
    assert(!flow.mqttDone(160000U, true, false));
    assert(flow.mqttDone(160000U + flow.serviceGap(), true, false));
    flow.advance(170000U); assert(flow.stage() == Stage::Cloud);
    flow.advance(180000U); assert(flow.stage() == Stage::Ota);
    flow.advance(190000U); assert(flow.stage() == Stage::Running);
    flow.advance(200000U); assert(flow.stage() == Stage::Running);
  }
  puts("boot policy: reset classification, L0-L3, offline admission, stability and rollover PASS");
}
