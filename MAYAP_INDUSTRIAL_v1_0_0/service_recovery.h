#pragma once
#include "config.h"
#include "runtime_recovery_policy.h"

namespace MayapServiceInternal {
struct Slot {
  uint32_t beat = 0U, ack = 0U, admitted = 0U, request = 0U;
  uint32_t isolateAt = 0U, isolated = 0U;
};
static Slot slots[4];
static uint32_t radioQuiesce = 0U;
static uint32_t otaQuiesced = 1U; // OTA not admitted yet has no I/O to drain.
static const char *const names[] = {"NETWORK", "MQTT", "CLOUD", "OTA"};
}
inline void mayapServiceBeat(MayapRecovery::Service service) {
  __atomic_store_n(&MayapServiceInternal::slots[static_cast<uint8_t>(service)].beat, millis(), __ATOMIC_RELEASE);
}
inline void mayapServiceAdmit(MayapRecovery::Service service) {
  mayapServiceBeat(service);
  __atomic_store_n(&MayapServiceInternal::slots[static_cast<uint8_t>(service)].admitted, 1U, __ATOMIC_RELEASE);
}
inline bool mayapServiceRecoveryRequested(MayapRecovery::Service service) {
  return __atomic_load_n(&MayapServiceInternal::slots[static_cast<uint8_t>(service)].request, __ATOMIC_ACQUIRE) != 0U;
}
inline void mayapServiceRecoveryComplete(MayapRecovery::Service service) {
  auto &slot = MayapServiceInternal::slots[static_cast<uint8_t>(service)];
  __atomic_store_n(&slot.request, 0U, __ATOMIC_RELEASE);
  __atomic_add_fetch(&slot.ack, 1U, __ATOMIC_ACQ_REL);
  mayapServiceBeat(service);
}
inline bool mayapServiceIsolated(MayapRecovery::Service service, uint32_t now) {
  auto &slot = MayapServiceInternal::slots[static_cast<uint8_t>(service)];
  if (!__atomic_load_n(&slot.isolated, __ATOMIC_ACQUIRE)) return false;
  if (MayapRecovery::age(now, __atomic_load_n(&slot.isolateAt, __ATOMIC_ACQUIRE)) < MayapRecovery::ISOLATE_PAUSE_MS) return true;
  __atomic_store_n(&slot.isolated, 0U, __ATOMIC_RELEASE);
  return false;
}
inline bool mayapRadioRecoveryRequested() {
  return __atomic_load_n(&MayapServiceInternal::radioQuiesce, __ATOMIC_ACQUIRE) != 0U;
}
inline bool mayapRadioOtaQuiesced() {
  return __atomic_load_n(&MayapServiceInternal::otaQuiesced, __ATOMIC_ACQUIRE) != 0U;
}
inline void mayapSetRadioOtaQuiesced(bool quiet) {
  __atomic_store_n(&MayapServiceInternal::otaQuiesced, quiet ? 1U : 0U, __ATOMIC_RELEASE);
}
inline bool mayapServiceSupervisorUpdate(uint32_t now, MayapRecovery::Service &failed) {
  static MayapRecovery::ServiceWatch watches[4];
  bool restart = false;
  for (uint8_t i = 0U; i < 4U; ++i) {
    auto &slot = MayapServiceInternal::slots[i];
    const auto action = watches[i].update(now,
        __atomic_load_n(&slot.admitted, __ATOMIC_ACQUIRE) != 0U,
        __atomic_load_n(&slot.beat, __ATOMIC_ACQUIRE),
        __atomic_load_n(&slot.ack, __ATOMIC_ACQUIRE), MayapRecovery::SERVICE_TIMEOUT_MS[i]);
    if (action == MayapRecovery::Action::Reinit || action == MayapRecovery::Action::Isolate) {
      __atomic_store_n(&slot.request, 1U, __ATOMIC_RELEASE);
      if (action == MayapRecovery::Action::Isolate) {
        __atomic_store_n(&slot.isolateAt, now, __ATOMIC_RELEASE);
        __atomic_store_n(&slot.isolated, 1U, __ATOMIC_RELEASE);
      }
      mayapSerialPrintf(false, "[SERVICE-RECOVERY] %s %s\n", MayapServiceInternal::names[i],
          action == MayapRecovery::Action::Reinit ? "OWNER REINIT REQUEST" : "ISOLATE");
    }
    if (action == MayapRecovery::Action::Restart) { failed = static_cast<MayapRecovery::Service>(i); restart = true; }
  }
  return restart;
}
