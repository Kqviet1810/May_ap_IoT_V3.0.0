#pragma once
#include "config.h"
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include "gpio_interrupts.h"

// Protocol v4: the proven GPIO + independent esp_timer command pulse;
// no RMT owns this bidirectional pad. Tiny replies with a
// six-pulse status frame (preamble, four state bits, XOR parity).
// GPIO41 and PB0 remain true GPIO open drain. The ESP internal 3.3 V pull-up
// is enabled as a safe idle fallback; the PCB must still use the external R8.
namespace MayapAttinyBusInternal {
constexpr uint8_t TX_QUEUE_SIZE = 8U;
constexpr uint8_t RESPONSE_EDGES = 12U;
constexpr uint8_t CAPTURE_EDGES = 14U; // Optional two command edges plus reply.
constexpr uint32_t RESPONSE_TIMEOUT_MS = 500UL;
constexpr uint32_t RETRY_GAP_MS = 200UL;
constexpr uint32_t FRAME_END_GAP_US = 30000UL;
constexpr uint32_t IDLE_GAP_US = 30000UL;
// A missing pull-up or a stuck-low peer must not leave a queued command
// pending forever: the controller needs a terminal result to raise E501.
constexpr uint32_t BUS_IDLE_TIMEOUT_MS = 500UL;

static uint8_t txQueue_[TX_QUEUE_SIZE]{};
static uint8_t txHead_ = 0U, txTail_ = 0U, txCount_ = 0U;
static uint8_t txCode_ = 0U, txAttempt_ = 0U;
static uint8_t resultCode_ = 0U, incomingCode_ = 0U;
static bool resultReady_ = false, resultAcked_ = false, driverReady_ = false;
static uint32_t txHoldUntil_ = 0U, txStartedAt_ = 0U;
static uint32_t responseStartedAt_ = 0U, retryAt_ = 0U;
static uint32_t idleBlockedAt_ = 0U;
static bool idleBlocked_ = false;
static esp_timer_handle_t txReleaseTimer_ = nullptr;
static uint32_t txPulseStartedUs_ = 0U;
static volatile uint32_t txPulseWidthUs_ = 0U;
enum class Phase : uint8_t { Idle, Sending, WaitingReply, RetryGap };
static Phase phase_ = Phase::Idle;

static volatile bool rxEnabled_ = false, edgeOverflow_ = false;
static volatile bool txPulseDone_ = false;
static volatile uint8_t edgeCount_ = 0U;
static volatile uint32_t edgeAtUs_[CAPTURE_EDGES]{};
static volatile uint32_t lastBusEdgeUs_ = 0U;
static portMUX_TYPE rxMux_ = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR busIsr(void *) {
  const uint32_t at = static_cast<uint32_t>(esp_timer_get_time());
  portENTER_CRITICAL_ISR(&rxMux_);
  lastBusEdgeUs_ = at;
  if (rxEnabled_) {
    if (edgeCount_ >= CAPTURE_EDGES) edgeOverflow_ = true;
    else edgeAtUs_[edgeCount_++] = at;
  }
  portEXIT_CRITICAL_ISR(&rxMux_);
}
inline void releaseBus() {
  (void)gpio_set_level(static_cast<gpio_num_t>(PIN_ATTINY_BUS), 1U);
}
inline bool busHigh() {
  // The driver owns the pad directly; bypass Arduino's periman/log wrapper.
  return gpio_get_level(static_cast<gpio_num_t>(PIN_ATTINY_BUS)) != 0;
}
void txReleaseCallback(void *) {
  releaseBus();
  txPulseWidthUs_ = static_cast<uint32_t>(esp_timer_get_time()) - txPulseStartedUs_;
  __atomic_store_n(&txPulseDone_, true, __ATOMIC_RELEASE);
}
inline bool commandPulseValid() {
  // Exact LINKFIX command windows; reject a valid-looking ACK if the actual
  // emitted LOW could have been interpreted as a different command.
  static const uint16_t minimumMs[] = {0U, 20U, 47U, 78U, 124U, 185U, 270U, 390U};
  static const uint16_t maximumMs[] = {0U, 38U, 65U, 105U, 160U, 240U, 350U, 510U};
  const uint32_t width = txPulseWidthUs_;
  return width >= minimumMs[txCode_] * 1000UL && width <= maximumMs[txCode_] * 1000UL;
}
inline bool reached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}
inline void clearEdges() {
  portENTER_CRITICAL(&rxMux_);
  edgeCount_ = 0U;
  edgeOverflow_ = false;
  portEXIT_CRITICAL(&rxMux_);
}
inline bool queuedOrActive(uint8_t code) {
  if (phase_ != Phase::Idle && txCode_ == code) return true;
  for (uint8_t i = 0U, p = txHead_; i < txCount_; ++i) {
    if (txQueue_[p] == code) return true;
    p = static_cast<uint8_t>((p + 1U) % TX_QUEUE_SIZE);
  }
  return false;
}
inline void finish(bool ok, uint8_t flags = 0U) {
  if (txReleaseTimer_ != nullptr && esp_timer_is_active(txReleaseTimer_))
    (void)esp_timer_stop(txReleaseTimer_);
  releaseBus();
  __atomic_store_n(&txPulseDone_, true, __ATOMIC_RELEASE);
#if MAYAP_DIAGNOSTIC_SERIAL
  if (ok) mayapSerialPrintf(false, "[ATTINY-BUS] ACK cmd=%u low=%luus edges=%u\n",
      static_cast<unsigned>(txCode_), static_cast<unsigned long>(txPulseWidthUs_),
      static_cast<unsigned>(edgeCount_));
#endif
  rxEnabled_ = false;
  clearEdges();
  resultCode_ = txCode_;
  resultAcked_ = ok;
  resultReady_ = true;
  incomingCode_ = ok ? static_cast<uint8_t>(ATTINY_MSG_STATUS_BASE + flags) : 0U;
  txCode_ = 0U;
  idleBlocked_ = false;
  phase_ = Phase::Idle;
}
#if MAYAP_DIAGNOSTIC_SERIAL
inline void logFailure(const char *reason) {
  mayapSerialPrintf(false, "[ATTINY-BUS] FAIL reason=%s cmd=%u attempt=%u edges=%u overflow=%u low=%luus\n",
                reason, static_cast<unsigned>(txCode_),
                static_cast<unsigned>(txAttempt_),
                static_cast<unsigned>(edgeCount_), edgeOverflow_ ? 1U : 0U,
                static_cast<unsigned long>(txPulseWidthUs_));
}
#else
inline void logFailure(const char *) {}
#endif
inline void retryOrFinish(uint32_t now, const char *reason) {
  logFailure(reason);
  rxEnabled_ = false;
  clearEdges();
  if (txAttempt_ >= ATTINY_BUS_MAX_RETRY) finish(false);
  else { retryAt_ = now + RETRY_GAP_MS; phase_ = Phase::RetryGap; }
}
inline bool decodeFrame(uint8_t &flags) {
  uint32_t edge[CAPTURE_EDGES];
  portENTER_CRITICAL(&rxMux_);
  const uint8_t count = edgeCount_;
  const bool overflow = edgeOverflow_;
  if (count >= RESPONSE_EDGES && count <= CAPTURE_EDGES && !overflow) {
    for (uint8_t i = 0U; i < count; ++i) edge[i] = edgeAtUs_[i];
  }
  portEXIT_CRITICAL(&rxMux_);
  // A complete reply has 12 edges, optionally preceded by our two TX edges.
  // An odd count must never be shifted into a seemingly complete frame.
  if ((count != RESPONSE_EDGES && count != CAPTURE_EDGES) || overflow) return false;
  const uint8_t base = count - RESPONSE_EDGES;
  const uint32_t preamble = edge[base + 1U] - edge[base];
  if (preamble < 45000UL || preamble > 75000UL) return false;
  uint8_t parity = 0U;
  flags = 0U;
  for (uint8_t i = 1U; i < 6U; ++i) {
    const uint32_t gap = edge[base + i * 2U] - edge[base + i * 2U - 1U];
    if (gap < 8000UL || gap > 25000UL) return false;
    const uint32_t width = edge[base + i * 2U + 1U] - edge[base + i * 2U];
    uint8_t bit;
    if (width >= 6000UL && width <= 16000UL) bit = 0U;
    else if (width >= 22000UL && width <= 40000UL) bit = 1U;
    else return false;
    if (i < 5U) { flags |= static_cast<uint8_t>(bit << (i - 1U)); parity ^= bit; }
    else if (bit != parity) return false;
  }
  return true;
}
inline void startAttempt(uint32_t now) {
  if (!driverReady_ || txReleaseTimer_ == nullptr) {
    logFailure("GPIO_TIMER_IRAM_INIT");
    finish(false);
    return;
  }
  clearEdges();
  rxEnabled_ = true;
  __atomic_store_n(&txPulseDone_, false, __ATOMIC_RELEASE);
  ++txAttempt_;
  txPulseWidthUs_ = 0U;
  txPulseStartedUs_ = static_cast<uint32_t>(esp_timer_get_time());
  txStartedAt_ = millis();
  if (gpio_set_level(static_cast<gpio_num_t>(PIN_ATTINY_BUS), 0U) != ESP_OK ||
      esp_timer_start_once(txReleaseTimer_,
          static_cast<uint64_t>(ATTINY_COMMAND_WIDTH_MS[txCode_]) * 1000ULL) != ESP_OK) {
    releaseBus();
    __atomic_store_n(&txPulseDone_, true, __ATOMIC_RELEASE);
    retryOrFinish(now, "TX_START");
    return;
  }
  responseStartedAt_ = txStartedAt_ + ATTINY_COMMAND_WIDTH_MS[txCode_];
  phase_ = Phase::Sending;
}
inline void startNext(uint32_t now) {
  if (txCount_ == 0U || resultReady_ || !reached(now, txHoldUntil_)) {
    idleBlocked_ = false;
    return;
  }
  if ((driverReady_ && !__atomic_load_n(&txPulseDone_, __ATOMIC_ACQUIRE)) ||
      !busHigh() ||
      static_cast<uint32_t>(micros() - lastBusEdgeUs_) < IDLE_GAP_US) {
    if (!idleBlocked_) {
      idleBlocked_ = true;
      idleBlockedAt_ = now;
    } else if (static_cast<uint32_t>(now - idleBlockedAt_) >= BUS_IDLE_TIMEOUT_MS) {
      txCode_ = txQueue_[txHead_];
      txHead_ = static_cast<uint8_t>((txHead_ + 1U) % TX_QUEUE_SIZE);
      --txCount_;
      txAttempt_ = 0U;
      logFailure("BUS_NOT_IDLE");
      finish(false);
    }
    return;
  }
  idleBlocked_ = false;
  txCode_ = txQueue_[txHead_];
  txHead_ = static_cast<uint8_t>((txHead_ + 1U) % TX_QUEUE_SIZE);
  --txCount_;
  txAttempt_ = 0U;
  startAttempt(now);
}
} // namespace MayapAttinyBusInternal

inline void mayapAttinyBusBegin() {
  using namespace MayapAttinyBusInternal;
  // Set the output latch HIGH before enabling open drain: HIGH means release.
  (void)gpio_set_level(static_cast<gpio_num_t>(PIN_ATTINY_BUS), 1U);
  gpio_config_t busConfig{};
  busConfig.pin_bit_mask = 1ULL << PIN_ATTINY_BUS;
  busConfig.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  busConfig.pull_up_en = GPIO_PULLUP_ENABLE;
  busConfig.pull_down_en = GPIO_PULLDOWN_DISABLE;
  busConfig.intr_type = GPIO_INTR_DISABLE;
  driverReady_ = gpio_config(&busConfig) == ESP_OK;
  if (driverReady_) releaseBus();

  esp_timer_create_args_t timerArgs{};
  timerArgs.callback = txReleaseCallback;
  timerArgs.dispatch_method = ESP_TIMER_TASK;
  timerArgs.name = "attiny-tx-release";
  if (driverReady_) {
    driverReady_ = esp_timer_create(&timerArgs, &txReleaseTimer_) == ESP_OK;
  }
  if (driverReady_) {
    driverReady_ = mayapEnsureCacheSafeGpioService() &&
        gpio_set_intr_type(static_cast<gpio_num_t>(PIN_ATTINY_BUS), GPIO_INTR_ANYEDGE) == ESP_OK &&
        gpio_isr_handler_add(static_cast<gpio_num_t>(PIN_ATTINY_BUS), busIsr, nullptr) == ESP_OK;
  }
  lastBusEdgeUs_ = micros();
  __atomic_store_n(&txPulseDone_, true, __ATOMIC_RELEASE);
#if MAYAP_DIAGNOSTIC_SERIAL
  mayapSerialPrintf(false, "[ATTINY-BUS] init gpio/timer/iram=%u idle=%u\n",
                driverReady_ ? 1U : 0U,
                busHigh() ? 1U : 0U);
#endif
}
inline bool mayapAttinyBusRequest(uint8_t code) {
  using namespace MayapAttinyBusInternal;
  if (code == 0U || code > ATTINY_MSG_MAX_COMMAND) return false;
  if (queuedOrActive(code)) return true;
  if (txCount_ >= TX_QUEUE_SIZE) return false;
  txQueue_[txTail_] = code;
  txTail_ = static_cast<uint8_t>((txTail_ + 1U) % TX_QUEUE_SIZE);
  ++txCount_;
  return true;
}
inline void mayapAttinyBusHoldTxUntil(uint32_t deadline) {
  MayapAttinyBusInternal::txHoldUntil_ = deadline;
}
inline bool mayapAttinyBusCommandPending(uint8_t code) {
  return code != 0U && code <= ATTINY_MSG_MAX_COMMAND &&
      MayapAttinyBusInternal::queuedOrActive(code);
}
inline void mayapAttinyBusUpdate(uint32_t now) {
  using namespace MayapAttinyBusInternal;
  now = millis(); // Caller timestamp may precede EEPROM/NVS/I2C work.
  switch (phase_) {
    case Phase::Idle: startNext(now); return;
    case Phase::Sending:
      if (!__atomic_load_n(&txPulseDone_, __ATOMIC_ACQUIRE)) {
        if (static_cast<uint32_t>(now - txStartedAt_) >
            ATTINY_COMMAND_WIDTH_MS[txCode_] + RESPONSE_TIMEOUT_MS + 200UL) {
          logFailure("TX_TIMEOUT");
          finish(false);
        }
        return;
      }
      // Timestamp the response from actual GPIO release, not a nominal width
      // or late foreground sample. Capture has remained enabled throughout.
      responseStartedAt_ = txStartedAt_ + txPulseWidthUs_ / 1000UL;
      phase_ = Phase::WaitingReply;
      // Fall through: a whole response may have arrived while control saved.
      // Decode retained edges before applying the timeout.
      // fall through
    case Phase::WaitingReply: {
      uint8_t count;
      bool overflow;
      uint32_t lastEdge = 0U;
      portENTER_CRITICAL(&rxMux_);
      count = edgeCount_;
      overflow = edgeOverflow_;
      if (count != 0U) lastEdge = edgeAtUs_[count - 1U];
      portEXIT_CRITICAL(&rxMux_);
      if (overflow) { retryOrFinish(now, "RX_OVERFLOW"); return; }
      // Silence during LOW is not an end-of-frame: a legal ONE lasts up
      // to 40 ms, longer than our 30 ms HIGH end gap. In particular the
      // final parity LOW leaves 13 edges until Tiny releases the wire.
      if (busHigh() && count >= RESPONSE_EDGES && count <= CAPTURE_EDGES &&
          static_cast<uint32_t>(micros() - lastEdge) >= FRAME_END_GAP_US) {
        uint8_t flags = 0U;
        if (!commandPulseValid()) retryOrFinish(now, "TX_PULSE_WIDTH");
        else if (decodeFrame(flags)) finish(true, flags);
        else retryOrFinish(now, "FRAME_INVALID");
        return;
      }
      if (static_cast<uint32_t>(now - responseStartedAt_) >= RESPONSE_TIMEOUT_MS)
        retryOrFinish(now, commandPulseValid() ? "RESPONSE_TIMEOUT" : "TX_PULSE_WIDTH");
      return;
    }
    case Phase::RetryGap:
      if (!reached(now, retryAt_)) return;
      if (busHigh() &&
          static_cast<uint32_t>(micros() - lastBusEdgeUs_) >= IDLE_GAP_US) {
        startAttempt(now);
      } else if (static_cast<uint32_t>(now - retryAt_) >= BUS_IDLE_TIMEOUT_MS) {
        logFailure("RETRY_BUS_NOT_IDLE");
        finish(false);
      }
      return;
  }
}
inline bool mayapAttinyBusTakeResult(uint8_t &code, bool &acked) {
  using namespace MayapAttinyBusInternal;
  if (!resultReady_) return false;
  code = resultCode_;
  acked = resultAcked_;
  resultReady_ = false;
  return true;
}
inline uint8_t mayapAttinyBusPollIncoming() {
  using namespace MayapAttinyBusInternal;
  const uint8_t code = incomingCode_;
  incomingCode_ = 0U;
  return code;
}
