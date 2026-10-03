# Thermal control V2 — adversarial review fixes

Branch `codex/thermal-control-v2`, PR #2, based on main `9569fcc`. No main merge, Cloudflare deploy, OTA or physical flash. This is a review/commissioning candidate, not a claim of achieved temperature accuracy.

## HARDWARE CONFIRMED

GPIO1 drives **both heater SSRs simultaneously**. Each SSR feeds two 4 kW heaters; the only controllable actuator is **one logical 16 kW bank**. GPIO1 OFF is 0 kW; GPIO1 ON is 16 kW. `HEATER_GROUP_COUNT` is fixed at one and defining `MAYAP_HEATER_SSR_B_PIN` fails compilation. The generic scheduler's two-channel path is future-only host code; it has no production output or claimed hardware. No B GPIO has been assigned. `runtime.heaterPower` stays the requested average of the whole 16 kW bank; 50% is 8 kW average, not one independently driven SSR.

Production path: paired RS485 T/RH profile → existing median-3/IIR-3/8 float filter → sample-driven PID (beta=1) → requested 0–100% average → one-bank pulse-density scheduler → OutputArbiter → GPIO1 → two SSRs together. Only the arbiter writes the heater GPIO. E115 HeaterNotHeating and SensorFrozen count every millisecond of GPIO1 ON as a full-bank millisecond; no half weighting. The actual watchdog body is compiled into a host test: 60 s accumulated ON triggers E115, 60 s OFF does not contribute, and sensor loss resets the evidence. Safety OFF and contactor/fan interlocks retain priority over scheduler phase. AutoTune remains Tyreus-Luyben, with its requested percentage passing through the same one-bank scheduler (30% = 4.8 kW average).

## ADVERSARIAL REVIEW FIXES

- **Anti-windup sticking:** the old conditional integrator discarded an entire I step if it crossed 0 or max power, leaving 3–4% requested even with PV about 2.55°C above SP. A step moving from inside the actuator range across a boundary now projects I onto that exact boundary. While already saturated, integration only proceeds toward the valid range. The test reproduces a 3–5% starting output at SP30/PV32.55, proves frozen OLD remains positive, and checks NEW reaches 0 over 3,000 cycles at dt 1/2/5/10 s, including millis wrap and max power 5/50/100%. Sensor/permit reset, upper/lower saturation and boundary crossing are hard gates. Kp=18, Ki=0.8, Kd=45, beta=1 remain unchanged; beta support is only a capability, not a proven improvement.
- **Single-bank correction:** OutputRequest/State/Arbiter contain one heater signal. Test mode and runtime status use the same GPIO1 bank state. GPIO1 ON counts as 100% instantaneous 16 kW for E115; the scheduler uses percentage only as a time-average request.
- **Quantum sweep:** the scheduler's hard minimum is 300 ms, and production defaults to 300 ms based on the 300/500/1000 ms sweep below. It never makes shortened catch-up pulses after task lateness; stalls, safety cuts, zero and NaN clear credit. No phase-angle or mains half-cycle control was added.
- **Paired sensor profile:** AUTO tests both raw T and raw RH against X10/RH-X10, X100/RH-X10 and native SHT30 T+RH. It requires six consecutive CRC-valid uniquely matching pairs before heating. Native 30902/39321 locks to ~37.52°C/60%; X100 3751/600 retains 37.51°C through filter and PID. A locked profile never switches after faults/reconnect. Explicit `MAYAP_SENSOR_PROFILE` (0=AUTO, 1/2/3 as above) replaces the split temperature/humidity flags, with a compile error for obsolete flags.
- **CI policy:** unit/integration, real E115, actual GPIO, format, pulse-energy/timing, and the SP30 anti-windup case are hard regression gates. The full uncalibrated plant grid remains an experimental CSV artifact and CI warning; failing physical-performance targets are not hidden or relaxed.

AUTO temperature identification assumes a real startup temperature of 10–60°C. Raw-only identification cannot universally distinguish out-of-envelope values. Likewise, a native RH raw value of 600 is plausibly ~0.9% and cannot be distinguished from an RH-X10 encoding by magnitude alone once native profile is locked. Actual module register map and commissioning raw readings must establish its format. Native data order on I2C is not assumed to equal the module's RS485 register order; firmware retains the existing RH-then-T Modbus order. After lock, invalid T/RH yields Sensor Fault and heater OFF; plausible upward raw T reaches EmergencyHigh immediately. CRC retry/staleness behavior is unchanged. Main Web/HMI display stays at one decimal; service/internal paths retain native resolution.

## Same-plant OLD/NEW performance experiment

[plant.csv](thermal-v2/plant.csv) has 1,536 deterministic, three-hour runs: 384 OLD fixed-10 s-window cases and 384 cases per NEW 300/500/1000 ms quantum. Each path sees identical uncalibrated lumped plant, noise, 2 s sensor cadence, resolution, median/IIR, ambient, delay and disturbances. The 100 ms integration step resolves all three quanta. OLD PID/window bodies are frozen from main `9569fcc` and SHA-256-guarded; this is a common-filter comparison, **not a full historical firmware binary replay**. Both switch one 16 kW bank. Plants have 0.18/0.6/1.6 MJ/K capacity, 120/180/300 W/K loss and an 8 s heater/air lag. Dead times: 5/15/30/60 s. Sensor resolution: 0.1/0.01°C. Scenarios include cold start, vent, door, ambient rise, noise, one bad sample, sensor loss/recovery, safety cut/resume and setpoint step. Final 30 min supply steady metrics.

Cold-start means over 288 cases per path (temperatures in °C; transitions are the mean of each run's worst one-hour count):

| Quantum | Mean MAE | Mean P95 | Mean ripple | Mean overshoot | Worst MAE | Mean worst SSR transitions/h | Max SSR transitions/h |
|---|---:|---:|---:|---:|---:|---:|---:|
| OLD 10 s | 0.24376 | 0.52265 | 0.77477 | 2.39341 | 2.17743 | 474 | 720 |
| NEW 300 ms | **0.24056** | **0.51390** | **0.74621** | 2.41306 | 2.24689 | 2698 | 7869 |
| NEW 500 ms | 0.24332 | 0.52141 | 0.75867 | 2.41452 | 2.27165 | 1619 | 4717 |
| NEW 1000 ms | 0.25072 | 0.53972 | 0.79057 | 2.41892 | 2.29978 | 807 | 2362 |

NEW 300 ms has the lowest mean MAE/P95/ripple, better low-duty resolution, and a 4.8 kJ full-bank packet instead of 8/16 kJ. Its mean overshoot is slightly worse than OLD and it switches the SSR substantially more often; its suitability depends on physical zero-cross SSR rating/temperature and electrical commissioning. No PID gain was tuned to the model.

| SP | OLD mean MAE | NEW 300 mean MAE | NEW 500 mean MAE | NEW 1000 mean MAE |
|---:|---:|---:|---:|---:|
| 30°C | 0.1581 | **0.1469** | 0.1470 | 0.1555 |
| 32°C | 0.2037 | **0.1963** | 0.2007 | 0.2087 |
| 35°C | **0.2754** | 0.2764 | 0.2794 | 0.2869 |
| 37.5°C | **0.3379** | 0.3427 | 0.3462 | 0.3517 |

OLD is still better at 35°C and 37.5°C mean MAE; NEW is not a universal thermal improvement. Original adversarial case SP30/ambient28/dead30/res0.01 had **2.547232°C NEW MAE before the anti-windup fix** in the prior 250 ms-step/1000 ms-quantum model. With only the PID fix and the same 250 ms-step/1000 ms quantum, MAE is **0.113949°C**. In the updated 100 ms sweep the chosen 300 ms case is **0.086605°C MAE**, +0.051566°C bias, 1.564% tail requested power; frozen OLD is 0.123133°C MAE. The 100 ms sweep is a separate timing experiment and should not be treated as the same binary run.

**Acceptance targets are still missed: 0/1536 runs pass all MAE, P95, ripple, overshoot and settling criteria.** The model is uncalibrated; modeled cold-start overshoot is especially high. CI retains this failure as an experimental warning. It cannot establish real chamber stability, sensor absolute accuracy or spatial uniformity.

## Low-duty, timing and safety gates

[low-duty.csv](thermal-v2/low-duty.csv) includes 0.1/0.25/0.5/1/2/3/5/10/20/50/75/100%, each at 120/600/3600 s and 300/500/1000 ms. It reports requested/delivered average, absolute energy error, maximum no-heat interval and transitions/hour. A full GPIO1 pulse is 16 kW, so one 300/500/1000 ms packet is 4.8/8/16 kJ. At 0.5% over 120 s, 300 ms delivers 0.5%, 500 ms 0.4167%, 1000 ms 0%; all converge to 0.5% over 600/3600 s. At 1% over 3600 s all deliver 1%; maximum no-heat intervals are 29.7/49.5/99 s and transitions/hour 239/143/71. At 3600 s the maximum energy error over all requested duties is 0/1.6/9.6 kJ respectively. Finite-horizon error is bounded to one packet; there is no drift. Worst-case 100%-to-alternating switch patterns can reach ~12,000/7,200/3,600 transitions/hour. Actual scheduler jitter tests guarantee no accidental 20/50/100 ms catch-up pulse, no stale heat credit, and immediate safety OFF. Physical SSR transition/thermal endurance remains to be checked.

E115/full-bank, 16 PID and 15 AutoTune actual heating-path safety cuts, native T+RH and X10/X100 AUTO locks, locked-format rejection, 3,000-cycle anti-windup, NaN/clamps, 1,000,000 variable-demand slots and millis wrap are tested. `maxHeaterPower=50` means 50% average of 16 kW. AutoTune's 30% request delivers 30% average through the same scheduler in the actual MachineController harness. Mechanical relay wear counters do not count pulse-rated SSR edges.

The existing config invariant is `highTempAlarm >= targetTemp + HIGH_ALARM_GAP_C` and `emergencyTemp >= highTempAlarm + EMERGENCY_ABOVE_HIGH_C`; lowering SP to 30°C does **not** automatically lower stored high/emergency thresholds (defaults 38.2/39.0°C). Review these limits manually before low-temperature operation. This PR does not change safety setpoints or stored schema.

## Test/CI and reproduction

`python3 tools/test_thermal_control.py --sanitize --report-dir /tmp/mayap-thermal-report` runs all hard thermal tests and generates both CSVs. `--require-targets` intentionally fails while uncalibrated performance targets are missed. Reliability CI runs the hard suite and uploads metrics, with a warning for experimental misses. All existing Node/account/protocol, actual runtime buses, EEPROM/notes, browser/workerd and firmware DEV/PROD builds must remain green before merge. No real ESP32 temperature-control test is claimed.

## REMAINING RISKS

- Confirm GPIO1 drives both SSR inputs on the physical board as specified, their zero-cross behavior, rating, heatsinking, contactor and 16 kW electrical loading. High burst transition rates make this a commissioning blocker until measured.
- Confirm the RS485 module register map and six-sample paired AUTO lock at known T/RH. Raw-only AUTO is not a universal profile detector outside the stated envelope; overlapping plausible raw values cannot be rejected reliably.
- Current default/stored PID gains are uncharacterized on the 12 m³ chamber. The uncalibrated model misses all joint performance targets and NEW is slightly worse than OLD in some 35/37.5°C cases. Run safe physical response identification/AutoTune before accepting thermal performance.
- High/emergency thresholds saved for 37.5°C can remain too high at SP30°C; operator configuration review is required. This PR deliberately does not change the safety policy.
- A single probe's stable reading does not prove ±0.1°C absolute accuracy or spatial uniformity throughout the chamber. Independent calibrated probes and a long soak are needed.
- HMI/Web retain the legacy `pidCycleSec` field for EEPROM compatibility; it no longer controls burst timing. UI semantics need separate review.
