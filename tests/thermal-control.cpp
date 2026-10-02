#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
using std::isfinite;

enum class ControlMode : uint8_t { OnOff, Pid };
enum class AutoTuneState : uint8_t { Idle, Running, Success, Failed };
struct MachineConfig {
  ControlMode controlMode = ControlMode::Pid;
  float kp = 20, ki = 0.04f, kd = 60;
  uint8_t maxHeaterPower = 100, autotuneRelayPowerPercent = 30;
  float tempHysteresis = 0.2f, autotuneBandC = 0.2f;
};
constexpr float PI = 3.14159265358979323846f;
constexpr float PID_D_FILTER_TAU_SEC = 5.0f;
constexpr float AUTOTUNE_STABILITY_FRACTION = 0.20f;
constexpr uint8_t AUTOTUNE_REQUIRED_CYCLES = 3;
constexpr uint32_t AUTOTUNE_MAX_MS = 2700000;
constexpr uint32_t AUTOTUNE_PHASE_MAX_MS = 900000;
constexpr uint32_t AUTOTUNE_MIN_PERIOD_MS = 10000;
constexpr float AUTOTUNE_MIN_AMPLITUDE_C = 0.10f;
uint32_t elapsedMs(uint32_t now, uint32_t then) { return now - then; }
float clampFloat(float x, float lo, float hi) { return std::max(lo, std::min(hi, x)); }
void sanitizeMachineConfig(MachineConfig &) {}
#include "../MAYAP_INDUSTRIAL_v4_0_0/thermal_control.h"

int main() {
  MachineConfig cfg, result;
  ThermalController pid;
  float pv = 25;
  for (uint32_t ms = 1000; ms <= 7200000; ms += 2000) {
    const float output = pid.updateOnNewSample(ms, 37.5f, pv, cfg, true);
    assert(std::isfinite(output) && output >= 0 && output <= 100);
    pv += 2 * (25 + 0.2f * output - pv) / 180;
  }
  assert(std::fabs(pv - 37.5f) < 0.5f);
  const float before = pid.output();
  cfg.kp = 25;
  pid.applyConfigBumpless(7200000, 37.5f, pv, cfg);
  assert(std::fabs(before - pid.output()) < 0.001f);
  assert(pid.updateOnNewSample(7202000, 37.5f, NAN, cfg, true) == 0);
  assert(pid.updateOnNewSample(7204000, NAN, 37, cfg, true) == 0);
  // Sustained saturation must not accumulate unlimited integral; disabled is OFF.
  pid.reset();
  for (uint32_t ms = 1000; ms < 200000; ms += 2000)
    assert(pid.updateOnNewSample(ms, 37.5f, 10, cfg, true) == 100);
  assert(pid.updateOnNewSample(200000, 37.5f, 10, cfg, false) == 0);

  RelayAutoTune tune;
  cfg.controlMode = ControlMode::OnOff;
  tune.configure(37.5f);
  tune.start(1000, 36.9f);
  bool done = false;
  for (uint32_t ms = 41000; ms <= 401000; ms += 40000) {
    const float input = ((ms - 41000) / 40000) % 2 == 0 ? 38.1f : 36.9f;
    done = tune.update(ms, input, cfg, result);
    if (ms < 361000) assert(!done); // warmup discarded + 3 complete cycles
    if (done) break;
  }
  assert(done && tune.state() == AutoTuneState::Success && tune.power() == 0);
  assert(result.controlMode == ControlMode::Pid);
  assert(result.kp > 0 && result.kp <= 100 && result.ki > 0 && result.ki <= 20);
  assert(result.kd > 0 && result.kd <= 200);
  // Restart must forget prior successes and fail cold/no-response with heater OFF.
  tune.start(1000, 25);
  assert(!tune.update(AUTOTUNE_PHASE_MAX_MS + 1000, 25, cfg, result));
  assert(tune.state() == AutoTuneState::Failed && tune.power() == 0);
  tune.start(1000, 36.9f);
  for (uint32_t i = 0; i < 16; ++i) {
    // Alternating large/small excursions never form three repeatable cycles.
    float input = i % 2 ? (i % 4 == 1 ? 35.5f : 37.2f) : (i % 4 == 0 ? 39.5f : 37.8f);
    assert(!tune.update(41000 + i * 40000, input, cfg, result));
  }
  assert(tune.running());
  tune.abort();
  assert(tune.power() == 0);
  std::puts("Thermal host tests: PID limits/bumpless/filter, model settling, tune warmup/stability/timeout OK");
}
