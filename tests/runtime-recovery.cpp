#include <cassert>
#include <cstdint>
#include <cstdio>
#include "../MAYAP_INDUSTRIAL_v4_0_0/runtime_recovery_policy.h"
using namespace MayapRecovery;
int main() {
  ServiceWatch watch;
  assert(watch.update(900000, false, 0, 0, 30000) == Action::None);
  assert(watch.update(30000, true, 0, 0, 30000) == Action::None);
  assert(watch.update(30001, true, 0, 0, 30000) == Action::Reinit);
  assert(watch.update(90000, true, 0, 0, 30000) == Action::None);
  assert(watch.update(90001, true, 0, 0, 30000) == Action::Isolate);
  assert(watch.update(330000, true, 0, 0, 30000) == Action::None);
  assert(watch.update(330001, true, 0, 0, 30000) == Action::Restart);
  assert(watch.update(330002, true, 330002, 1, 30000) == Action::None);
  assert(watch.update(360003, true, 330002, 1, 30000) == Action::Reinit);
  ServiceWatch offline;
  for (uint32_t now = 100; now < 10000000; now += 100)
    assert(offline.update(now, true, now, 0, 30000) == Action::None);
  ServiceWatch rollover;
  assert(rollover.update(0xFFFFF000U, true, 0xFFFFF000U, 0, 30000) == Action::None);
  assert(rollover.update(0xFFFFF000U + 30001U, true, 0xFFFFF000U, 0, 30000) == Action::Reinit);
  assert(rollover.update(0xFFFFF000U + 330001U, true, 0xFFFFF000U, 0, 30000) == Action::Isolate);
  assert(rollover.update(0xFFFFF000U + 330002U, true, 0xFFFFF000U, 0, 30000) == Action::Restart);
  WifiRecovery wifi;
  for (unsigned i = 0; i < 5; ++i) wifi.failure(100);
  assert(!wifi.wanted(100));
  wifi.failure(100);
  assert(wifi.wanted(100));
  wifi.started(100);
  assert(!wifi.wanted(100 + WIFI_OFFLINE_MS)); // old outage cannot preempt a new join
  for (unsigned i = 0; i < 6; ++i) wifi.failure(101);
  assert(!wifi.wanted(120099));
  assert(wifi.wanted(120100));
  wifi.started(120100);
  assert(!wifi.isolate());
  wifi.started(240100);
  assert(wifi.isolate());
  wifi.success(240101);
  assert(!wifi.isolate() && !wifi.wanted(1000000));
  WifiRecovery longOutage;
  longOutage.offline(0xFFFFFF00U);
  assert(!longOutage.wanted(0xFFFFFF00U + WIFI_OFFLINE_MS - 1U));
  assert(longOutage.wanted(0xFFFFFF00U + WIFI_OFFLINE_MS));
  std::puts("Runtime policy: admission, owner recovery, isolation, escalation, offline and rollover PASS");
}
