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

  std::puts("Thermal host tests: production PID limits, bumpless config, invalid-input reset and anti-windup PASS");
}
