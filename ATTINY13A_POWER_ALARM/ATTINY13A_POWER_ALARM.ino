/*
  ATtiny13A Power Alarm - PROD v2.1 LINKFIX
  Goal: fit ATtiny13A 1 KB Flash, minimum battery drain, event-driven only.

  PB0 = ESP32 BUS (open-drain, external pull-up 3.3 V)
  PB1 = Siren output, active HIGH
  PB2 = ESP 3.3 V sense, digital PCINT wake
  PB3 = 9 V sense, digital PCINT wake
  PB4 = unused -> output LOW
  PB5 = RESET / ISP

  Compile target: ATtiny13A, F_CPU = 1.2 MHz (9.6 MHz RC / 8)
*/

#ifndef F_CPU
#define F_CPU 1200000UL
#endif

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <avr/eeprom.h>
#include <util/delay.h>

#if F_CPU != 1200000UL
#error "Use F_CPU=1200000UL"
#endif

constexpr uint8_t PROTOCOL_VERSION = 4U;

#define BUS       _BV(PB0)
#define SIREN     _BV(PB1)
#define ESPPIN    _BV(PB2)
#define V9PIN     _BV(PB3)
#define SPARE     _BV(PB4)
#define PWRMASK   (ESPPIN | V9PIN)

/* Low nibble is intentionally identical to the status sent to ESP32. */
#define F_BATCH    0x01
#define F_9VFAULT  0x02
#define F_EMERG    0x04
#define F_ACTIVITY 0x08
#define F_ESPLOST  0x10
#define F_BUSFAULT 0x20
#define F_PERSIST  (F_BATCH | F_ACTIVITY)

static uint8_t flags;
static uint8_t powerPins;

uint8_t EEMEM eeState;
uint8_t EEMEM eeInv;

/* Disable watchdog as early as possible after watchdog reset. */
void wdtOffEarly(void) __attribute__((naked, section(".init3")));
void wdtOffEarly(void) {
  MCUSR = 0;
  wdt_disable();
}

static inline void dms(uint16_t ms) {
  while (ms--) _delay_ms(1);
}

static inline uint8_t busLowNow(void) {
  return !(PINB & BUS);
}

static inline void busRelease(void) {
  PORTB &= (uint8_t)~BUS;
  DDRB  &= (uint8_t)~BUS;
}

static inline void busDriveLow(void) {
  PORTB &= (uint8_t)~BUS;
  DDRB  |= BUS;
}

static inline void sirenUpdate(void) {
  if ((flags & F_EMERG) ||
      ((flags & (F_BATCH | F_ACTIVITY)) && (flags & F_ESPLOST)))
    PORTB |= SIREN;
  else
    PORTB &= (uint8_t)~SIREN;
}

/* EEPROM: value + inverse, invalid/blank => fail-safe armed. */
static uint8_t loadPersist(void) {
  uint8_t a = eeprom_read_byte(&eeState);
  uint8_t b = eeprom_read_byte(&eeInv);
  if ((uint8_t)(a ^ b) == 0xFF && !(a & (uint8_t)~F_PERSIST)) return a;
  return F_PERSIST;
}

/* No redundant write; boot-time inverse validation protects recovery. */
static void persistBit(uint8_t bit, uint8_t on) {
  uint8_t oldv = flags & F_PERSIST;
  uint8_t newv = on ? (uint8_t)(oldv | bit)
                    : (uint8_t)(oldv & (uint8_t)~bit);
  if (newv == oldv) return;

  eeprom_update_byte(&eeState, newv);
  eeprom_update_byte(&eeInv, (uint8_t)~newv);
  flags = (uint8_t)((flags & (uint8_t)~F_PERSIST) | newv);
}

/* 3-sample majority filter, only called at boot or after an actual pin change. */
static uint8_t stablePower(void) {
  uint8_t a = PINB & PWRMASK;
  dms(2);
  uint8_t b = PINB & PWRMASK;
  dms(2);
  uint8_t c = PINB & PWRMASK;
  return (uint8_t)((a & b) | (a & c) | (b & c));
}

static void setPower(uint8_t p) {
  powerPins = p;

  if (p & ESPPIN) flags &= (uint8_t)~F_ESPLOST;
  else            flags |= F_ESPLOST;

  if (p & V9PIN) flags &= (uint8_t)~F_9VFAULT;
  else           flags |= F_9VFAULT;

  sirenUpdate();
}

static inline void processPower(void) {
  if ((PINB & PWRMASK) == powerPins) return;
  uint8_t p = stablePower();
  if (p != powerPins) setPower(p);
}

static uint8_t decode(uint16_t w) {
  if (w >= 20  && w <= 38)  return 1;
  if (w >= 47  && w <= 65)  return 2;
  if (w >= 78  && w <= 105) return 3;
  if (w >= 124 && w <= 160) return 4;
  if (w >= 185 && w <= 240) return 5;
  if (w >= 270 && w <= 350) return 6;
  if (w >= 390 && w <= 510) return 7;
  return 0;
}

static uint16_t readPulse(void) {
  /*
    Measure in 0.25 ms quanta instead of counting 1 ms software loops.
    This is much closer to the tested UNO micros()-based receiver and
    reduces boundary errors on the 20..510 ms command windows.
  */
  uint16_t q = 0;
  while (busLowNow()) {
    _delay_us(250);
    wdt_reset();
    if (++q > 2120U) return 531U;   /* >530 ms */
  }
  return (uint16_t)((q + 2U) >> 2); /* round to nearest ms */
}

static void sendStatus(void) {
  uint8_t f = flags & 0x0F;
  uint8_t parity = 0;

  dms(40);
  busDriveLow(); dms(60); busRelease(); dms(15);

  for (uint8_t i = 0; i < 5; ++i) {
    uint8_t one;
    if (i < 4) {
      one = (f >> i) & 1;
      parity ^= one;
    } else {
      one = parity;
    }

    busDriveLow();
    dms(one ? 30 : 10);
    busRelease();
    if (i != 4) dms(15);
  }
}

static void apply(uint8_t cmd) {
  switch (cmd) {
    case 1: persistBit(F_BATCH, 1);    break;
    case 2: persistBit(F_BATCH, 0);    break;
    case 3: flags |= F_EMERG;          break;
    case 4: flags &= (uint8_t)~F_EMERG; break;
    case 5:                            break;
    case 6: persistBit(F_ACTIVITY, 1); break;
    case 7: persistBit(F_ACTIVITY, 0); break;
  }
  sirenUpdate();
}

static void handleBus(void) {
  /* Recovery from a previously stuck-low BUS. */
  if (flags & F_BUSFAULT) {
    if (!busLowNow()) flags &= (uint8_t)~F_BUSFAULT;
    return;
  }

  if (!busLowNow()) return;

  /*
    Communication-critical section.
    V2 left PB2/PB3 pin-change interrupts active while measuring and
    transmitting BUS pulses. Any edge/noise there could insert ISR latency
    into the protocol timing. Freeze interrupts for the whole transaction;
    processPower() samples the real pin levels immediately afterwards, so a
    power transition is not lost.
  */
  cli();
  GIMSK &= (uint8_t)~_BV(PCIE);

  wdt_enable(WDTO_2S);
  uint16_t w = readPulse();

  if (w == 531U) {
    flags |= F_BUSFAULT;
  } else {
    uint8_t cmd = decode(w);
    if (cmd) {
      apply(cmd);
      wdt_reset();
      sendStatus();
    }
  }

  wdt_disable();

  /* Clear edges caused by this transaction before re-arming wake. */
  GIFR = _BV(PCIF);
  GIMSK |= _BV(PCIE);
  sei();
}

/* Wake-only interrupt. Current pin levels are the source of truth. */
ISR(PCINT0_vect) {}

static void sleepNow(void) {
  wdt_disable();
  cli();

  /* Do not sleep over work that arrived just before CLI. */
  if ((busLowNow() && !(flags & F_BUSFAULT)) ||
      ((PINB & PWRMASK) != powerPins)) {
    sei();
    return;
  }

  sleep_enable();
#if defined(BODS) && defined(BODSE)
  sleep_bod_disable();
#endif
  sei();
  sleep_cpu();
  sleep_disable();
}

void setup(void) {
  cli();

  /* 9.6 MHz internal RC / 8 = 1.2 MHz. */
  CLKPR = _BV(CLKPCE);
  CLKPR = _BV(CLKPS1) | _BV(CLKPS0);

  PORTB = 0;
  DDRB = SIREN | SPARE;          /* siren LOW, PB4 fixed LOW */
  busRelease();                  /* PB0 high impedance */
  DDRB &= (uint8_t)~PWRMASK;     /* PB2/PB3 inputs */

  ADCSRA &= (uint8_t)~_BV(ADEN);
  ACSR |= _BV(ACD);
  PRR |= _BV(PRADC) | _BV(PRTIM0);

  flags = loadPersist() & F_PERSIST;
  setPower(stablePower());

  PCMSK = BUS | PWRMASK;
  GIFR = _BV(PCIF);
  GIMSK |= _BV(PCIE);

  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sei();
}

void loop(void) {
  handleBus();
  processPower();
  sleepNow();
}

// CI uses avr-libc directly; Arduino/MicroCore supplies its own main.
#ifndef ARDUINO
int main(void) {
  setup();
  for (;;) loop();
}
#endif
