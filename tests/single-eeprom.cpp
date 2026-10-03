#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
static uint32_t clockMs = 0, pollPauseMs = 0;
uint32_t millis() { return clockMs; }
uint32_t elapsedMs(uint32_t now, uint32_t before) { return now - before; }
uint32_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
void vTaskDelay(uint32_t ms) { clockMs += ms + (ms==1?pollPauseMs:0); }
bool mayapI2cLock(uint32_t) { return true; }
void mayapI2cUnlock() {}
void mayapI2cReport(uint8_t, bool) {}
int constrain(int value, int minimum, int maximum) {
  return std::max(minimum, std::min(value, maximum));
}
struct FakeWire {
  std::array<uint8_t, 65536> memory{};
  std::vector<uint8_t> tx, rx;
  size_t cursor = 0, writes = 0, probes = 0;
  uint16_t word = 0;
  uint8_t chip = 0;
  int cutAfter = -1;
  bool off = false, wp = false; uint32_t writeCycleMs=0,busyUntil=0;
  FakeWire() { memory.fill(0xFF); }
  void beginTransmission(uint8_t address) { chip = address; tx.clear(); }
  size_t write(uint8_t byte) {
    if (tx.size() >= 128) return 0;
    tx.push_back(byte); return 1;
  }
  size_t write(const uint8_t *data, size_t length) {
    size_t n = 0;
    while (n < length && write(data[n])) ++n;
    return n;
  }
  uint8_t endTransmission(bool) {
    if(tx.empty())++probes;
    if (off || chip != 0x50 || (writeCycleMs&&static_cast<int32_t>(clockMs-busyUntil)<0)) return 2;
    if (tx.size() >= 2) word = static_cast<uint16_t>((tx[0] << 8) | tx[1]);
    if (tx.size() > 2) {
      ++writes;
      for (size_t i = 2; i < tx.size(); ++i) {
        if (cutAfter == 0) { off = true; return 2; }
        if (cutAfter > 0) --cutAfter;
        // Actual C512 page wrap. Incorrect driver geometry corrupts readback.
        const uint16_t address = static_cast<uint16_t>((word & 0xFF80) |
                                                   ((word + i - 2) & 0x7F));
        if (!wp) memory[address] = tx[i];
      }
      if(writeCycleMs)busyUntil=clockMs+writeCycleMs;
    }
    return 0;
  }
  size_t requestFrom(uint8_t address, uint8_t length, bool) {
    rx.clear(); cursor = 0;
    if (off || address != 0x50 || (writeCycleMs&&static_cast<int32_t>(clockMs-busyUntil)<0)) return 0;
    for (uint8_t i = 0; i < length; ++i) rx.push_back(memory[word++]);
    return rx.size();
  }
  int available() { return static_cast<int>(rx.size() - cursor); }
  int read() { return available() ? rx[cursor++] : -1; }
} Wire;
#include "actual-single-eeprom.inc"

int main() {
  ExternalEeprom24xx driver;
  assert(driver.begin());
  std::array<uint8_t, 400> pattern{}, readback{};
  for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<uint8_t>(i);
  assert(driver.writeBytes(0xF001, pattern.data(), pattern.size()));
  assert(driver.readBytes(0xF001, readback.data(), readback.size()));
  assert(pattern == readback);
  uint8_t byte = 123, result = 0;
  assert(driver.writeBytes(0xFFFF, &byte, 1));
  assert(driver.readBytes(0xFFFF, &result, 1) && result == byte);
  assert(!driver.writeBytes(0xFFFF, pattern.data(), 2));
  assert(driver.writeBytes(0xFFFF, &byte, 0));
  ReminderSet original{}, updated{}, loaded{};
  original.items[0].day = 4;
  std::snprintf(original.items[0].label, CUSTOM_REMINDER_LABEL_LEN, "Kiem tra khay");
  updated = original;
  updated.items[1].day = 10;
  std::snprintf(updated.items[1].label, CUSTOM_REMINDER_LABEL_LEN, "Ghi chu ngay 10");
  ReminderStore store;
  assert(store.saveReminders(original, loaded));
  const auto baseline = Wire.memory;
  const size_t initialWrites = Wire.writes;
  assert(store.saveReminders(original, loaded));
  assert(Wire.writes == initialWrites); // Unchanged notes don't wear EEPROM.
  for (int cut = 0; cut <= static_cast<int>(sizeof(ReminderRecordV1)); ++cut) {
    Wire.memory = baseline; Wire.off = false; Wire.cutAfter = cut;
    ReminderStore before;
    assert(before.loadReminders(loaded));
    const bool ok = before.saveReminders(updated, loaded);
    Wire.off = false; Wire.cutAfter = -1;
    ReminderStore reboot;
    assert(reboot.loadReminders(loaded));
    assert(loaded.items[0].day == 4);
    assert(loaded.items[1].day == (ok ? 10 : 0));
  }
  Wire.memory = baseline; Wire.wp = true;
  ReminderStore protectedStore;
  assert(!protectedStore.saveReminders(updated, loaded));
  Wire.wp = false;
  ReminderStore afterFailure;
  assert(afterFailure.loadReminders(loaded) && loaded.items[1].day == 0);
  assert(afterFailure.saveReminders(updated, loaded));
  ReminderStore afterReboot;
  assert(afterReboot.loadReminders(loaded) && loaded.items[1].day == 10);
  Wire.memory[EEPROM_ADDR_REMINDERS_B + 20] ^= 1;
  ReminderStore corrupted;
  assert(corrupted.loadReminders(loaded) && loaded.items[1].day == 0);
  ReminderSet empty{};
  assert(corrupted.saveReminders(empty, loaded));
  ReminderStore cleared;
  assert(cleared.loadReminders(loaded) && loaded.items[0].day == 0);
  for (uint8_t i = 0; i < MAX_CUSTOM_REMINDERS; ++i) {
    updated.items[i].day = i + 1;
    std::snprintf(updated.items[i].label, CUSTOM_REMINDER_LABEL_LEN,
                  "Ngày %u: kiểm tra nhiệt độ và độ ẩm", i + 1);
  }
  assert(cleared.saveReminders(updated, loaded));
  ReminderStore vietnamese;
  assert(vietnamese.loadReminders(loaded));
  assert(std::memcmp(&loaded, &updated, sizeof(loaded)) == 0);
  // ACK polling regression is a driver property, independent of any feature.
  // A delayed task resume must probe the chip once more before declaring timeout.
  uint8_t timingPayload[64]{}, timingReadback[64]{};
  Wire.memory.fill(0xFF);Wire.writeCycleMs=5;clockMs=1000;Wire.busyUntil=clockMs;pollPauseMs=25;
  const bool delayedWrite=driver.writeBytes(0xF100,timingPayload,sizeof(timingPayload));
  if(!delayedWrite)fprintf(stderr,"Ack poll regression: driver write addr=0xF100 reason=%u cycle=5ms scheduler_pause=25ms\n",driver.lastWriteTrace().reason);
  assert(delayedWrite);
  pollPauseMs=0;assert(driver.readBytes(0xF100,timingReadback,sizeof(timingReadback)));
  assert(std::memcmp(timingPayload,timingReadback,sizeof(timingPayload))==0);
  for(uint32_t pause:{0U,1U,19U,20U,21U,50U,250U})for(uint32_t begun:{1000U,UINT32_MAX-8U}){
    clockMs=begun;Wire.busyUntil=begun;pollPauseMs=pause;
    assert(driver.writeBytes(0xF180,timingPayload,sizeof(timingPayload)));
  }
  Wire.writeCycleMs=1000;clockMs=1000;Wire.busyUntil=clockMs;pollPauseMs=25;Wire.probes=0;
  assert(!driver.writeBytes(0xF200,timingPayload,sizeof(timingPayload)));
  // writeBytes() has finite whole-operation retries. A later retry can fail
  // at the initial address phase while the chip is still busy, so the final
  // trace reason is not required to remain the first attempt's poll-timeout.
  assert(Wire.probes<=22);
  pollPauseMs=0;Wire.writeCycleMs=0;
  std::puts("C512 ACK polling: generic driver, 5ms write cycle, delayed task wake-up and millis wrap PASS");
  std::puts("C512: page/Wire boundary, 0xFFFF/range, reminders reboot/clear/no-op, WP and CRC fallback PASS");
}
