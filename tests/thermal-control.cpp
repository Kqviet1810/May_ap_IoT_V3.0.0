#include "thermal-fixture.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_control.h"

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
  tune.update(1000, 37.3f, cfg, result);
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
  assert(tune.result().gainScale == 1.0f);

  // A mathematically identified TL result that exceeds the supported PID
  // envelope must fail instead of being uniformly shrunk and auto-saved.
  RelayAutoTune rangeTune;
  MachineConfig rejected = cfg;
  rangeTune.configure(37.5f);
  rangeTune.start(1000, 36.9f);
  rangeTune.update(1000, 37.3f, cfg, rejected);
  for (uint32_t ms = 121000; ms <= 1201000; ms += 120000) {
    const float input = ((ms - 121000) / 120000) % 2 == 0 ? 38.1f : 36.9f;
    (void)rangeTune.update(ms, input, cfg, rejected);
    if (!rangeTune.running()) break;
  }
  assert(rangeTune.state() == AutoTuneState::Failed);
  assert(rangeTune.reason() == AutoTuneReason::InvalidGains);
  assert(rangeTune.result().gainScale > 1.0f);
  assert(rangeTune.power() == 0);

  // Restart must forget prior successes and fail cold/no-response with heater OFF.
  tune.start(1000, 25);
  assert(!tune.update(AUTOTUNE_PHASE_MAX_MS + 1000, 25, cfg, result));
  assert(tune.state() == AutoTuneState::Failed && tune.power() == 0);
  tune.start(1000, 36.9f);
  tune.update(1000, 37.3f, cfg, result);
  for (uint32_t i = 0; i < 16; ++i) {
    // Alternating large/small excursions never form three repeatable cycles.
    float input = i % 2 ? (i % 4 == 1 ? 35.5f : 37.2f) : (i % 4 == 0 ? 39.5f : 37.8f);
    assert(!tune.update(41000 + i * 40000, input, cfg, result));
  }
  assert(tune.running());
  tune.abort();
  assert(tune.power() == 0);
  std::puts("Thermal host tests: PID limits/bumpless/filter, model settling, exact-TL gain-envelope rejection, tune warmup/stability/timeout OK");
}
