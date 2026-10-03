// Real EEPROM load/migration/CRC/sanitize paths; only the I/O is byte-backed.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>
using std::isfinite;
int constrain(int v,int lo,int hi){return std::max(lo,std::min(hi,v));}
float clampFloat(float v,float lo,float hi){return std::max(lo,std::min(hi,v));}
#include "actual-config.inc"
#include "../MAYAP_INDUSTRIAL_v1_0_0/heater_burst_scheduler.h"
constexpr uint8_t HEATER_GROUP_COUNT=1;
struct ConfigStore {
  uint8_t bytes[1024];
  bool ready_=true,configCacheValid_=false,configCurrentIsA_=false;
  uint32_t configSequence_=0;
  PackedMachineConfigV1 configPayload_{};
  ConfigStore(){std::memset(bytes,0xff,sizeof(bytes));}
  template<class T> bool readRecord(uint16_t addr,T &r) const {
    assert(addr+sizeof(r)<=sizeof(bytes));std::memcpy(&r,bytes+addr,sizeof(r));return true;
  }
  template<class T> bool writeRecord(uint16_t addr,const T &r) {
    assert(addr+sizeof(r)<=sizeof(bytes));std::memcpy(bytes+addr,&r,sizeof(r));return true;
  }
#include "actual-config-load.inc"
};
static std::vector<bool> waveform(const MachineConfig &loaded) {
  // No legacy field may affect timing. This is the exact production initializer.
#include "actual-burst-member.inc"
  std::vector<bool> result;
  for(uint32_t t=0;t<60000;t+=5) result.push_back(heaterBurst_.update(t,30,true).groupA);
  assert(loaded.pidCycleSec>=1 && loaded.pidCycleSec<=60);
  return result;
}
template<class Record> void verifyLegacy(uint16_t schema,uint16_t cycle) {
  MachineConfig input;input.pidCycleSec=cycle;
  const PackedMachineConfigV1 packed=packConfig(input);
  Record r{};r.magic=CONFIG_MAGIC;r.schema=schema;r.size=sizeof(r);r.sequence=9;
  std::memcpy(&r.payload,&packed,sizeof(r.payload));
  r.crc=mcCrc32(reinterpret_cast<const uint8_t *>(&r),offsetof(Record,crc));
  ConfigStore store;std::memcpy(store.bytes+EEPROM_ADDR_CONFIG_A,&r,sizeof(r));
  MachineConfig loaded;assert(store.loadConfig(loaded));
  assert(loaded.pidCycleSec==cycle && loaded.targetTemp==input.targetTemp);
  assert(packConfig(loaded).pidCycleSec==cycle);
  assert(!loaded.adaptiveThermalBalanceEnabled);
  MachineConfig reference;assert(waveform(loaded)==waveform(reference));
  // Corrupt CRC must not admit a configuration.
  store.bytes[offsetof(Record,crc)]^=1;assert(!store.loadConfig(loaded));
}
int main(){
  static_assert(CONFIG_SCHEMA==13,"Append-only adaptive opt-in schema");
  static_assert(HEATER_BURST_QUANTUM_MS==300,"Commissioning candidate stays 300 ms");
  for(uint16_t cycle:{1U,10U,60U}) {
    verifyLegacy<ConfigRecordV1>(13,cycle);
    verifyLegacy<ConfigRecordLegacyV12>(12,cycle);
    verifyLegacy<ConfigRecordLegacyV3>(3,cycle);
    verifyLegacy<ConfigRecordLegacyV4>(4,cycle);
    verifyLegacy<ConfigRecordLegacyV5>(5,cycle);
    verifyLegacy<ConfigRecordLegacyV6>(6,cycle);
    verifyLegacy<ConfigRecordLegacyV7>(7,cycle);
    verifyLegacy<ConfigRecordLegacyV8>(8,cycle);
    verifyLegacy<ConfigRecordLegacyV9>(9,cycle);
    verifyLegacy<ConfigRecordLegacyV10>(10,cycle);
    verifyLegacy<ConfigRecordLegacyV11>(11,cycle);
  }
  MachineConfig low;low.targetTemp=30;sanitizeMachineConfig(low);
  assert(low.highTempAlarm==38.2f && low.emergencyTemp==39.0f);
  low.highTempAlarm=29;low.emergencyTemp=29;sanitizeMachineConfig(low);
  assert(low.highTempAlarm>=low.targetTemp+HIGH_ALARM_GAP_C);
  assert(low.emergencyTemp>=low.highTempAlarm+EMERGENCY_ABOVE_HIGH_C);
  std::puts("Actual EEPROM schemas 3..13: legacy cycle 1/10/60 loads, CRC rejected, constant timing; SP30 retains 38.2/39 safety thresholds PASS");
}
