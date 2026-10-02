// Actual ESP driver and Tiny decoder on a simulated wired-AND GPIO bus.
// Independent timer task / IRAM capture continue while control is stalled.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <algorithm>
#include <vector>
#define IRAM_ATTR
#define MAYAP_DIAGNOSTIC_SERIAL 0
#define portMUX_INITIALIZER_UNLOCKED 0
using portMUX_TYPE = int;
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define portENTER_CRITICAL_ISR(x) ((void)(x))
#define portEXIT_CRITICAL_ISR(x) ((void)(x))
constexpr int HIGH=1, LOW=0, ESP_OK=0, ESP_FAIL=-1, ESP_TIMER_TASK=0;
constexpr int ESP_INTR_FLAG_IRAM=1, GPIO_MODE_INPUT_OUTPUT_OD=1;
constexpr int GPIO_PULLUP_ENABLE=1, GPIO_PULLDOWN_DISABLE=0;
constexpr int GPIO_INTR_DISABLE=0, GPIO_INTR_ANYEDGE=1;
using gpio_num_t = int;
struct gpio_config_t { uint64_t pin_bit_mask; int mode,pull_up_en,pull_down_en,intr_type; };
using esp_timer_handle_t = void *;
struct esp_timer_create_args_t { void (*callback)(void *); void *arg; int dispatch_method; const char *name; };
static uint64_t clockUs=1000000, lowStarted=0;
static int busLevel=HIGH, hostLevel=HIGH, peerLevel=HIGH;
static bool initFailure=false, txFailure=false, deferDone=false, timerActive=false;
static bool respond=true, badParity=false, overflowReply=false;
static uint8_t replyFlags=0;
static uint32_t measuredLowUs=0, timerDelayUs=0;
static uint32_t zeroUs=10000, oneUs=30000, replyGapUs=15000, preambleUs=60000;
static void (*timerCallback)(void *)=nullptr;
static void (*gpioHandler)(void *)=nullptr;
struct Event { uint64_t at; int level; bool done; };
static std::vector<Event> events;
static uint32_t millis() { return static_cast<uint32_t>(clockUs/1000); }
static uint32_t micros() { return static_cast<uint32_t>(clockUs); }
static int64_t esp_timer_get_time() { return clockUs; }
static int gpio_get_level(gpio_num_t) { return busLevel; }
#include "actual-tiny-decoder.inc"
static void updatePad() {
  const int level=hostLevel==LOW || peerLevel==LOW ? LOW : HIGH;
  if (busLevel!=level) { busLevel=level; if (gpioHandler) gpioHandler(nullptr); }
}
static void queueReply() {
  if (!respond || decode(static_cast<uint16_t>(measuredLowUs/1000))==0) return;
  uint64_t cursor=clockUs+48500; // Tiny EEPROM worst path plus 40 ms guard.
  events.push_back({cursor,LOW,false}); cursor+=preambleUs;
  events.push_back({cursor,HIGH,false}); cursor+=replyGapUs;
  uint8_t parity=0;
  for (unsigned i=0; i<5; ++i) {
    const uint8_t bit=i<4 ? (replyFlags>>i)&1U : parity^badParity;
    if (i<4) parity^=bit;
    events.push_back({cursor,LOW,false}); cursor+=bit ? oneUs : zeroUs;
    events.push_back({cursor,HIGH,false}); cursor+=replyGapUs;
  }
  if (overflowReply) for (unsigned i=0; i<4; ++i) {
    events.push_back({cursor,static_cast<int>(i%2),false}); cursor+=1000;
  }
}
static int gpio_set_level(gpio_num_t, unsigned level) {
  const int previous=hostLevel; hostLevel=level; updatePad();
  if (previous==HIGH && level==LOW) lowStarted=clockUs;
  if (previous==LOW && level==HIGH) {
    measuredLowUs=static_cast<uint32_t>(clockUs-lowStarted); queueReply();
  }
  return ESP_OK;
}
static int gpio_config(const gpio_config_t *cfg) {
  assert(cfg->mode==GPIO_MODE_INPUT_OUTPUT_OD && cfg->pull_up_en==GPIO_PULLUP_ENABLE);
  return initFailure ? ESP_FAIL : ESP_OK;
}
static int gpio_install_isr_service(int flags) { assert(flags==ESP_INTR_FLAG_IRAM); return ESP_OK; }
static int gpio_set_intr_type(gpio_num_t,int type) { assert(type==GPIO_INTR_ANYEDGE); return ESP_OK; }
static int gpio_isr_handler_add(gpio_num_t,void (*handler)(void *),void *) { gpioHandler=handler; return ESP_OK; }
static int esp_timer_create(const esp_timer_create_args_t *args,esp_timer_handle_t *handle) {
  assert(args->dispatch_method==ESP_TIMER_TASK);
  timerCallback=args->callback; *handle=reinterpret_cast<void *>(1); return ESP_OK;
}
static int esp_timer_start_once(esp_timer_handle_t,uint64_t width) {
  if (txFailure) return ESP_FAIL;
  assert(!timerActive); timerActive=true;
  events.push_back({clockUs+width+timerDelayUs,HIGH,true}); return ESP_OK;
}
static bool esp_timer_is_active(esp_timer_handle_t) { return timerActive; }
static int esp_timer_stop(esp_timer_handle_t) {
  timerActive=false;
  events.erase(std::remove_if(events.begin(),events.end(),[](const Event &e){return e.done;}),events.end());
  return ESP_OK;
}
static void advance(uint64_t target) {
  while (!events.empty() && events.front().at<=target) {
    const Event e=events.front(); events.erase(events.begin()); clockUs=e.at;
    if (e.done) { if (!deferDone) { timerActive=false; timerCallback(nullptr); } }
    else { peerLevel=e.level; updatePad(); }
  }
  clockUs=target;
}
#include "actual-attiny-config.inc"
#include "actual-gpio_interrupts.inc"
#include "actual-attiny_bus.inc"
#include "actual-boot-mailbox.inc"
#include "../MAYAP_INDUSTRIAL_v4_0_0/startup_output_policy.h"

static void reset() {
  using namespace MayapAttinyBusInternal;
  events.clear(); clockUs = 1000000; busLevel = hostLevel = peerLevel = HIGH;
  timerActive = false; timerDelayUs = 0;
  zeroUs=10000; oneUs=30000; replyGapUs=15000; preambleUs=60000;
  initFailure = txFailure = deferDone = badParity = overflowReply = false;
  respond = true; replyFlags = 0;
  txHead_ = txTail_ = txCount_ = txCode_ = txAttempt_ = incomingCode_ = 0;
  resultReady_ = resultAcked_ = driverReady_ = idleBlocked_ = false;
  txHoldUntil_ = 0; phase_ = Phase::Idle; rxEnabled_ = false;
  mayapAttinyBusBegin(); advance(clockUs + 31000);
}
static bool result(uint8_t expected) {
  uint8_t code = 0; bool ok = false;
  assert(mayapAttinyBusTakeResult(code, ok) && code == expected);
  return ok;
}
static void pollUntilResult() {
  for (unsigned i = 0; i < 400 && !MayapAttinyBusInternal::resultReady_; ++i) {
    mayapAttinyBusUpdate(millis()); advance(clockUs + 10000);
  }
  assert(MayapAttinyBusInternal::resultReady_);
}
struct Request {
  bool heaterSsr=true, heatMaster=true, turnLeft=true, turnRight=true, light=true, humidifier=true;
  bool circulationFan=true, ventFan=true, ventFanForceOn=true, ventFanBypassTiming=false;
  bool immediateMasterDrop=false, siren=true;
};
int main() {
  assert(!mayapBootOperationsReady());
  mayapBootReleaseHome(); // Coordinator exit request is not a displayed frame.
  assert(mayapBootHomeReleased() && !mayapBootOperationsReady());
  mayapBootAcknowledgeHomeFrame();
  assert(mayapBootOperationsReady());
  unsigned transfers = 0;
  for (uint8_t command = 1; command <= 7; ++command) for (uint8_t flags = 0; flags < 16; ++flags) {
    for (uint32_t stallMs : {0U, 300U, 600U, 1100U}) {
      reset(); replyFlags = flags;
      assert(mayapAttinyBusRequest(command));
      // A stale loop sample must not backdate physical TX/deadlines.
      mayapAttinyBusUpdate(0);
      assert(MayapAttinyBusInternal::txStartedAt_ == millis());
      assert(mayapAttinyBusCommandPending(command));
      advance(clockUs + stallMs * 1000ULL);
      pollUntilResult(); assert(!mayapAttinyBusCommandPending(command));
      assert(result(command));
      assert(measuredLowUs == ATTINY_COMMAND_WIDTH_MS[command] * 1000U);
      assert(mayapAttinyBusPollIncoming() == ATTINY_MSG_STATUS_BASE + flags);
      assert(busLevel == HIGH); ++transfers;
    }
  }
  // Poll while the final parity LOW is still in progress. 30 ms silence
  // does NOT mark a frame end: a legal ONE can remain LOW for 40 ms.
  for (uint32_t width : {22000U, 29999U, 30001U, 31000U, 32000U, 39999U, 40000U}) {
    for (uint8_t flags=0; flags<16; ++flags) {
      reset(); oneUs=width; replyFlags=flags;
      assert(mayapAttinyBusRequest(5)); pollUntilResult();
      assert(result(5));
      assert(mayapAttinyBusPollIncoming()==ATTINY_MSG_STATUS_BASE+flags);
      assert(MayapAttinyBusInternal::txAttempt_==1); ++transfers;
    }
  }
  for (uint32_t width : {6000U, 16000U}) for (uint32_t gap : {8000U, 25000U}) {
    reset(); zeroUs=width; oneUs=40000; replyGapUs=gap; preambleUs=75000; replyFlags=7;
    assert(mayapAttinyBusRequest(5)); pollUntilResult(); assert(result(5)); ++transfers;
  }
  for (uint32_t width : {16001U, 21999U, 40001U}) {
    reset(); oneUs=width; replyFlags=1;
    assert(mayapAttinyBusRequest(5)); pollUntilResult(); assert(!result(5));
  }
  reset(); respond = false; assert(mayapAttinyBusRequest(5)); pollUntilResult();
  assert(!result(5) && MayapAttinyBusInternal::txAttempt_ == ATTINY_BUS_MAX_RETRY);
  assert(mayapAttinyBusPollIncoming() == 0 && busLevel == HIGH);
  reset(); peerLevel = LOW; updatePad(); assert(mayapAttinyBusRequest(5)); pollUntilResult(); assert(!result(5));
  reset(); badParity = true; assert(mayapAttinyBusRequest(5)); pollUntilResult(); assert(!result(5));
  reset(); overflowReply = true; assert(mayapAttinyBusRequest(5)); pollUntilResult(); assert(!result(5));
  reset(); txFailure = true; assert(mayapAttinyBusRequest(5)); pollUntilResult(); assert(!result(5));
  assert(busLevel == HIGH);
  reset(); initFailure = true; mayapAttinyBusBegin(); assert(mayapAttinyBusRequest(5));
  pollUntilResult(); assert(!result(5));
  reset(); for (uint8_t c = 1; c <= 7; ++c) assert(mayapAttinyBusRequest(c));
  assert(mayapAttinyBusCommandPending(1) && mayapAttinyBusCommandPending(7));
  assert(!mayapAttinyBusCommandPending(0) && !mayapAttinyBusCommandPending(8));
  assert(mayapAttinyBusRequest(7)); assert(!mayapAttinyBusRequest(0)); assert(!mayapAttinyBusRequest(8));
  pollUntilResult();
  const uint8_t queued = MayapAttinyBusInternal::txCount_;
  advance(clockUs + 100000); mayapAttinyBusUpdate(0);
  assert(MayapAttinyBusInternal::txCount_ == queued);
  assert(!mayapAttinyBusCommandPending(1) && mayapAttinyBusCommandPending(2));
  assert(result(1));
  for (uint8_t c = 2; c <= 7; ++c) { pollUntilResult(); assert(result(c)); }
  reset(); clockUs = (1ULL << 32) - 50000; // capture timestamp wraps during reply
  assert(mayapAttinyBusRequest(5)); pollUntilResult(); assert(result(5));
  reset(); clockUs = ((1ULL << 32) - 100) * 1000; // millis wraps during command
  assert(mayapAttinyBusRequest(5)); pollUntilResult(); assert(result(5));
  reset(); deferDone = true; assert(mayapAttinyBusRequest(5));
  pollUntilResult(); assert(!result(5)); // Driver completion missing is bounded, not silently ignored.
  assert(busLevel == HIGH);
  reset(); timerDelayUs = 25000; assert(mayapAttinyBusRequest(5)); pollUntilResult(); assert(result(5));
  reset(); timerDelayUs = 60000; assert(mayapAttinyBusRequest(5)); pollUntilResult(); assert(!result(5));
  // Tiny decodes the stretched STATUS_QUERY as ACTIVITY_ON: no false query ACK.
  assert(decode(measuredLowUs / 1000) == 6 && mayapAttinyBusPollIncoming() == 0);
  for (bool cooling : {false, true}) for (bool siren : {false, true}) {
    Request r; mayapApplyStartupOutputPolicy(r, false, cooling, siren);
    assert(!r.heaterSsr && !r.heatMaster && !r.turnLeft && !r.turnRight && !r.light && !r.humidifier);
    assert(r.immediateMasterDrop && r.ventFan == cooling && r.circulationFan == cooling && r.siren == siren);
    assert(r.ventFanForceOn == cooling && r.ventFanBypassTiming == cooling);
  }
  Request ready; mayapApplyStartupOutputPolicy(ready, true, false, false);
  assert(ready.light && ready.heaterSsr && !ready.immediateMasterDrop);
  std::printf("Actual GPIO/Tiny decoder: %u transfers, control stalls, pulse drift, corruption, disconnect, TX failure, rollover and startup interlock OK\n", transfers);
}
