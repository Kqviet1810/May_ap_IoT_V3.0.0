#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <cstddef>
static uint32_t clockMs = 0U;
uint32_t millis() { return clockMs; }
uint32_t micros() { return clockMs * 1000U; }
uint32_t elapsedMs(uint32_t now, uint32_t then) { return now - then; }
bool timeReached(uint32_t now, uint32_t when) { return static_cast<int32_t>(now - when) >= 0; }
void mayapSerialPrintf(bool, const char *, ...) {}
constexpr int LOW=0, HIGH=1, INPUT_PULLUP=2, OUTPUT_OPEN_DRAIN=3, OUTPUT=4;
constexpr uint8_t PIN_I2C_SDA=1, PIN_I2C_SCL=2, PIN_RS485_RX=3, PIN_RS485_TX=4, PIN_RS485_DE_RE=5, SHT_UART_PORT=1;
constexpr uint8_t LCD_I2C_ADDRESS=0x3F, RTC_I2C_ADDRESS=0x68, EEPROM_I2C_ADDRESS=0x50;
constexpr uint32_t I2C_CLOCK_HZ=100000, I2C_TIMEOUT_MS=25, SERIAL_8N1=1;
static bool sdaStuck=false, sclStuck=false, lockAvailable=true, locked=false;
static int levels[8]={HIGH,HIGH,HIGH,HIGH,HIGH,LOW,HIGH,HIGH};
static unsigned pulses=0, releaseAfter=0, delaysUs=0;
void pinMode(int, int) {}
int digitalRead(int pin) {
  if (pin == PIN_I2C_SDA && sdaStuck) return LOW;
  if (pin == PIN_I2C_SCL && sclStuck) return LOW;
  return levels[pin];
}
void digitalWrite(int pin, int value) {
  if (pin == PIN_I2C_SCL && value == HIGH && levels[pin] == LOW) {
    ++pulses;
    if (releaseAfter && pulses >= releaseAfter) sdaStuck=false;
  }
  levels[pin]=value;
}
void delayMicroseconds(unsigned duration) { delaysUs += duration; }
bool mayapI2cLock(uint32_t timeout) { assert(timeout == 0); if (!lockAvailable) return false; assert(!locked); return locked=true; }
void mayapI2cUnlock() { assert(locked); locked=false; }
struct FakeWire {
  unsigned ends=0, begins=0, probes=0;
  bool present=true;
  void end() { assert(locked); ++ends; }
  bool begin(int sda, int scl, uint32_t hz) { assert(locked && sda==PIN_I2C_SDA && scl==PIN_I2C_SCL && hz==I2C_CLOCK_HZ); ++begins; return true; }
  void setTimeOut(uint32_t ms) { assert(ms==I2C_TIMEOUT_MS); }
  void beginTransmission(uint8_t) { assert(locked); ++probes; }
  uint8_t endTransmission(bool stop) { assert(stop); return present ? 0 : 2; }
} Wire;
#include "actual-i2c.inc"
static unsigned uartBegins=0, uartEnds=0, uartWrites=0;
static int replyMode=0;
static std::deque<uint8_t> uartRx;
static uint16_t crc16(const uint8_t *data, size_t length) {
  uint16_t crc=0xFFFF;
  for (size_t i=0; i<length; ++i) {
    crc ^= data[i];
    for (unsigned bit=0; bit<8; ++bit) crc = crc & 1 ? (crc>>1)^0xA001 : crc>>1;
  }
  return crc;
}
class HardwareSerial {
 public:
  explicit HardwareSerial(uint8_t port) { assert(port==SHT_UART_PORT); }
  void begin(uint32_t baud, uint32_t format, int rx, int tx) {
    assert(baud==9600 && format==SERIAL_8N1 && rx==PIN_RS485_RX && tx==PIN_RS485_TX);
    assert(levels[PIN_RS485_DE_RE]==LOW); ++uartBegins;
  }
  void end() { assert(levels[PIN_RS485_DE_RE]==LOW); ++uartEnds; uartRx.clear(); }
  int available() { return static_cast<int>(uartRx.size()); }
  int read() { if (uartRx.empty()) return -1; auto b=uartRx.front(); uartRx.pop_front(); return b; }
  int availableForWrite() { return 128; }
  size_t write(const uint8_t *data, size_t length) {
    assert(levels[PIN_RS485_DE_RE]==HIGH && length==8 && crc16(data,6)==static_cast<uint16_t>(data[6]|data[7]<<8));
    ++uartWrites;
    if (replyMode) {
      uint8_t frame[]={1,3,4,0x02,0x58,0x01,0x77,0,0}; // 60% RH, 37.5 C
      const uint16_t crc=crc16(frame,7);
      frame[7]=static_cast<uint8_t>(crc); frame[8]=static_cast<uint8_t>(crc>>8);
      if (replyMode==2) frame[8]^=1;
      for (auto byte : frame) uartRx.push_back(byte);
    }
    return length;
  }
};
#include "actual-uart.inc"
static void run(SHT485Industrial &sensor, uint32_t duration) {
  for (uint32_t i=0; i<duration; ++i) { ++clockMs; sensor.update(); }
}
int main() {
  clockMs=1000;
  lockAvailable=false; sdaStuck=true;
  mayapI2cSupervisorUpdate(clockMs);
  assert(Wire.ends==0);
  lockAvailable=true; sdaStuck=false; clockMs=2000;
  for (unsigned i=0; i<3; ++i) mayapI2cReport(LCD_I2C_ADDRESS,false);
  mayapI2cSupervisorUpdate(clockMs);
  assert(Wire.ends==0); // One missing device is not a shared bus hang.
  for (unsigned i=0; i<3; ++i) mayapI2cReport(RTC_I2C_ADDRESS,false);
  clockMs=3000; mayapI2cSupervisorUpdate(clockMs);
  assert(Wire.ends==1 && Wire.begins==1 && Wire.probes==3 && !locked);
  sdaStuck=true; releaseAfter=3; pulses=0;
  clockMs=4000; mayapI2cSupervisorUpdate(clockMs); assert(Wire.ends==1);
  clockMs=33000; mayapI2cSupervisorUpdate(clockMs);
  assert(Wire.ends==2 && pulses==3 && !sdaStuck && mayapI2cRecoveryEpoch()==2);
  sdaStuck=true; releaseAfter=0; pulses=0; delaysUs=0;
  clockMs=63000; mayapI2cSupervisorUpdate(clockMs);
  assert(Wire.ends==3 && pulses==9 && delaysUs<=105 && !locked);
  sclStuck=true; pulses=0; clockMs=93000; mayapI2cSupervisorUpdate(clockMs);
  assert(Wire.ends==4 && pulses<=1 && !locked);
  sdaStuck=sclStuck=false;
  clockMs=0;
  SHT485Industrial sensor;
  sensor.begin();
  for (unsigned i=0; i<25000 && !uartEnds; ++i) run(sensor,1);
  assert(uartEnds==1 && uartBegins==2 && !sensor.online() && !sensor.dataValid());
  const uint32_t recoveredAt=clockMs;
  run(sensor,29999); assert(uartEnds==1); // UART reinit cooldown.
  while (uartEnds<3 && clockMs<130000) run(sensor,1);
  assert(uartEnds==3);
  const unsigned beforeWrites=uartWrites;
  run(sensor,29999); assert(uartWrites==beforeWrites); // Isolated but still scheduled.
  replyMode=2; run(sensor,3000);
  assert(sensor.crcErrors()>0 && !sensor.dataValid());
  replyMode=1; run(sensor,7000);
  assert(sensor.online() && sensor.dataValid() && sensor.goodFrames()>=3);
  assert(std::fabs(sensor.temperatureC()-37.5f)<0.01f);
  replyMode=0; run(sensor,7000);
  assert(!sensor.online() && !sensor.dataValid());
  assert(clockMs-recoveredAt>30000 && levels[PIN_RS485_DE_RE]==LOW);
  std::puts("Actual I2C/UART: mutex, correlated errors, cooldown, <=9 clocks, stuck lines, CRC, reinit, isolate, reconnect and stale samples PASS");
}
