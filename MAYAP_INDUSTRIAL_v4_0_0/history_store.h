#pragma once

#include "config.h"
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <stdint.h>

// I2C mutex duoc tao/define trong file .ino truoc khi include header nay.
bool mayapI2cLock(uint32_t timeoutMs);
void mayapI2cUnlock();

struct MayapTemperatureHistoryPoint {
  uint32_t epoch = 0U;
  int16_t temperatureX10 = 0;
};

namespace MayapTemperatureHistoryInternal {

// Record 4 byte:
//   byte0: bucket[7:0]
//   byte1: bucket[11:8] | tempCode[3:0] << 4
//   byte2: tempCode[11:4]
//   byte3: CRC-8 (poly 0x07) cua 3 byte dau
// bucket = floor(epoch/300) mod 4096; tempCode = round(temp*10)+500.
// Slot vat ly = floor(epoch/300) mod TEMP_HISTORY_SLOT_COUNT (7 ngay tren C512).
// Khong metadata/index ghi dinh ky; moi slot duoc viet lai sau 7 ngay.
static uint32_t lastSampleBucket = UINT32_MAX;
static volatile uint32_t latestRtcEpoch = 0U;

inline uint8_t crc8(const uint8_t *data, size_t length) {
  uint8_t crc = 0U;
  while (length--) {
    crc ^= *data++;
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      crc = (crc & 0x80U) ? static_cast<uint8_t>((crc << 1U) ^ 0x07U)
                          : static_cast<uint8_t>(crc << 1U);
    }
  }
  return crc;
}

inline uint16_t addressForBucket(uint32_t absoluteBucket) {
  const uint16_t slot = static_cast<uint16_t>(absoluteBucket % TEMP_HISTORY_SLOT_COUNT);
  return static_cast<uint16_t>(EEPROM_ADDR_TEMP_HISTORY +
                               slot * TEMP_HISTORY_RECORD_BYTES);
}

inline bool readRaw(uint16_t address, uint8_t out[TEMP_HISTORY_RECORD_BYTES]) {
  if (!mayapI2cLock(I2C_STORAGE_LOCK_TIMEOUT_MS)) return false;
  Wire.beginTransmission(EEPROM_I2C_ADDRESS);
  Wire.write(static_cast<uint8_t>(address >> 8U));
  Wire.write(static_cast<uint8_t>(address & 0xFFU));
  if (Wire.endTransmission(false) != 0U) {
    mayapI2cUnlock();
    return false;
  }
  const size_t got = Wire.requestFrom(EEPROM_I2C_ADDRESS,
                                      static_cast<uint8_t>(TEMP_HISTORY_RECORD_BYTES),
                                      static_cast<uint8_t>(true));
  bool ok = got == TEMP_HISTORY_RECORD_BYTES;
  for (uint8_t i = 0U; i < TEMP_HISTORY_RECORD_BYTES && ok; ++i) {
    if (!Wire.available()) { ok = false; break; }
    out[i] = static_cast<uint8_t>(Wire.read());
  }
  mayapI2cUnlock();
  return ok;
}

inline bool writeRaw(uint16_t address, const uint8_t data[TEMP_HISTORY_RECORD_BYTES]) {
  // Dia chi history luon boi so 4 va record 4B, khong vuot page EEPROM.
  if ((address % EEPROM_PAGE_SIZE) > EEPROM_PAGE_SIZE - TEMP_HISTORY_RECORD_BYTES) return false;
  if (!mayapI2cLock(I2C_STORAGE_LOCK_TIMEOUT_MS)) return false;
  Wire.beginTransmission(EEPROM_I2C_ADDRESS);
  Wire.write(static_cast<uint8_t>(address >> 8U));
  Wire.write(static_cast<uint8_t>(address & 0xFFU));
  const size_t written = Wire.write(data, TEMP_HISTORY_RECORD_BYTES);
  const uint8_t err = Wire.endTransmission(true);
  if (written != TEMP_HISTORY_RECORD_BYTES || err != 0U) {
    mayapI2cUnlock();
    return false;
  }

  // ACK polling: AT24C32 tu ghi noi bo toi da ~5ms; gioi han 20ms giong driver
  // storage chinh. Chi mot record 4B/5phut, khong nam tren duong dieu khien nong.
  const uint32_t started = millis();
  bool ready = false;
  do {
    Wire.beginTransmission(EEPROM_I2C_ADDRESS);
    ready = Wire.endTransmission(true) == 0U;
    if (!ready) delay(1);
  } while (!ready && static_cast<uint32_t>(millis() - started) < EEPROM_WRITE_TIMEOUT_MS);
  mayapI2cUnlock();
  return ready;
}

inline bool encode(uint32_t absoluteBucket, float temperature,
                   uint8_t out[TEMP_HISTORY_RECORD_BYTES]) {
  if (!isfinite(temperature) || temperature < -20.0f || temperature > 100.0f) return false;
  const int32_t temp10 = lroundf(temperature * 10.0f);
  const int32_t codeSigned = temp10 + 500;
  if (codeSigned < 0 || codeSigned > 4095) return false;
  const uint16_t tag = static_cast<uint16_t>(absoluteBucket & 0x0FFFU);
  const uint16_t code = static_cast<uint16_t>(codeSigned);
  out[0] = static_cast<uint8_t>(tag & 0xFFU);
  out[1] = static_cast<uint8_t>(((tag >> 8U) & 0x0FU) | ((code & 0x0FU) << 4U));
  out[2] = static_cast<uint8_t>((code >> 4U) & 0xFFU);
  out[3] = crc8(out, 3U);
  return true;
}

inline bool decode(uint32_t absoluteBucket,
                   const uint8_t raw[TEMP_HISTORY_RECORD_BYTES],
                   MayapTemperatureHistoryPoint &out) {
  if (crc8(raw, 3U) != raw[3]) return false;
  const uint16_t tag = static_cast<uint16_t>(raw[0] | ((raw[1] & 0x0FU) << 8U));
  if (tag != static_cast<uint16_t>(absoluteBucket & 0x0FFFU)) return false;
  const uint16_t code = static_cast<uint16_t>(((raw[1] >> 4U) & 0x0FU) |
                                              (static_cast<uint16_t>(raw[2]) << 4U));
  const int16_t temp10 = static_cast<int16_t>(static_cast<int32_t>(code) - 500);
  if (temp10 < -200 || temp10 > 1000) return false;
  out.epoch = absoluteBucket * TEMP_HISTORY_SAMPLE_SEC;
  out.temperatureX10 = temp10;
  return true;
}

inline bool readBucket(uint32_t absoluteBucket, MayapTemperatureHistoryPoint &out) {
  uint8_t raw[TEMP_HISTORY_RECORD_BYTES]{};
  if (!readRaw(addressForBucket(absoluteBucket), raw)) return false;
  return decode(absoluteBucket, raw, out);
}

inline bool writeBucket(uint32_t absoluteBucket, float temperature) {
  uint8_t raw[TEMP_HISTORY_RECORD_BYTES]{};
  if (!encode(absoluteBucket, temperature, raw)) return false;
  return writeRaw(addressForBucket(absoluteBucket), raw);
}

}  // namespace MayapTemperatureHistoryInternal

inline void mayapTemperatureHistorySample(uint32_t rtcEpoch, float temperature,
                                          bool sensorUsable) {
  using namespace MayapTemperatureHistoryInternal;
  if (rtcEpoch == 0U) return;
  __atomic_store_n(&latestRtcEpoch, rtcEpoch, __ATOMIC_RELEASE);
  if (!sensorUsable || !isfinite(temperature)) return;

  const uint32_t bucket = rtcEpoch / TEMP_HISTORY_SAMPLE_SEC;
  if (bucket == lastSampleBucket) return;

  // Sau reboot co the slot hien tai da duoc ghi truoc do. Doc kiem tra truoc
  // de khong ghi lai cung mot page vo ich. Neu doc fail/record cu thi ghi mau moi.
  MayapTemperatureHistoryPoint existing{};
  const bool alreadyStored = readBucket(bucket, existing);
  lastSampleBucket = bucket;  // history la best-effort; loi I2C khong duoc hammer moi 5ms.
  if (alreadyStored) return;
  (void)writeBucket(bucket, temperature);
}

inline uint32_t mayapTemperatureHistoryLatestEpoch() {
  return __atomic_load_n(&MayapTemperatureHistoryInternal::latestRtcEpoch,
                         __ATOMIC_ACQUIRE);
}

inline bool mayapTemperatureHistoryReadBucket(uint32_t absoluteBucket,
                                              MayapTemperatureHistoryPoint &out) {
  return MayapTemperatureHistoryInternal::readBucket(absoluteBucket, out);
}

// 0=I2C error, 1=empty/old CRC or bucket, 2=valid sample.
inline uint8_t mayapTemperatureHistoryReadStatus(uint32_t bucket,
                                                  MayapTemperatureHistoryPoint &out) {
  uint8_t raw[TEMP_HISTORY_RECORD_BYTES]{};
  if (!MayapTemperatureHistoryInternal::readRaw(
          MayapTemperatureHistoryInternal::addressForBucket(bucket), raw)) return 0U;
  return MayapTemperatureHistoryInternal::decode(bucket, raw, out) ? 2U : 1U;
}
