# Thermal control V2 — adversarial review fixes

Branch `codex/thermal-control-v2`, PR #2, based on main `9569fcc`. No main merge, Cloudflare deploy, OTA or physical flash. This is a review/commissioning candidate, not a claim of achieved temperature accuracy.

## HARDWARE CONFIRMED

GPIO1 drives **both heater SSRs simultaneously**. Latest hardware description: eight elements, four on each side of the circulation fan, total 16 kW; the only controllable actuator is **one logical 16 kW bank**. GPIO1 OFF is 0 kW; GPIO1 ON is 16 kW. `HEATER_GROUP_COUNT` is fixed at one and defining `MAYAP_HEATER_SSR_B_PIN` fails compilation. The generic scheduler's two-channel path is future-only host code; it has no production output or claimed hardware. No B GPIO has been assigned. `runtime.heaterPower` stays the requested average of the whole 16 kW bank; 50% is 8 kW average, not one independently driven SSR.

Production path: paired RS485 T/RH profile → existing median-3/IIR-3/8 float filter → sample-driven PID (beta=1) → requested 0–100% average → one-bank pulse-density scheduler → OutputArbiter → GPIO1 → two SSRs together. Only the arbiter writes the heater GPIO. E115 HeaterNotHeating and SensorFrozen count every millisecond of GPIO1 ON as a full-bank millisecond; no half weighting. The actual watchdog body is compiled into a host test at 5 ms control cadence/300 ms quantum: 900 s accumulated ON triggers E115 for each 5/10/30/50/100% request; OFF does not contribute, and sensor loss resets the evidence. Safety OFF and contactor/fan interlocks retain priority over scheduler phase. AutoTune remains Tyreus-Luyben, with its requested percentage passing through the same one-bank scheduler (30% = 4.8 kW average).

## ADVERSARIAL REVIEW FIXES

- **Anti-windup sticking:** the old conditional integrator discarded an entire I step if it crossed 0 or max power, leaving 3–4% requested even with PV about 2.55°C above SP. A step moving from inside the actuator range across a boundary now projects I onto that exact boundary. While already saturated, integration only proceeds toward the valid range. The test reproduces a 3–5% starting output at SP30/PV32.55, proves frozen OLD remains positive, and checks NEW reaches 0 over 3,000 cycles at dt 1/2/5/10 s, including millis wrap and max power 5/50/100%. Sensor/permit reset, upper/lower saturation and boundary crossing are hard gates. Kp=18, Ki=0.8, Kd=45, beta=1 remain unchanged; beta support is only a capability, not a proven improvement.
- **Single-bank correction:** OutputRequest/State/Arbiter contain one heater signal. Test mode and runtime status use the same GPIO1 bank state. GPIO1 ON counts as 100% instantaneous 16 kW for E115; the scheduler uses percentage only as a time-average request.
- **Quantum sweep:** the scheduler's hard minimum is 300 ms, and the explicit `HEATER_BURST_QUANTUM_MS` in config.h sets 300 ms as a **provisional commissioning candidate**, not a physically validated final setting. The production member passes both bank count and quantum explicitly. It never makes shortened catch-up pulses after task lateness; stalls, safety cuts, zero and NaN clear credit. No phase-angle or mains half-cycle control was added.
- **Paired sensor profile:** AUTO tests both raw T and raw RH against X10/RH-X10, X100/RH-X10 and native SHT30 T+RH. It requires six consecutive CRC-valid uniquely matching pairs before heating. Native 30902/39321 locks to ~37.52°C/60%; X100 3751/600 retains 37.51°C through filter and PID. A locked profile never switches after faults/reconnect. Explicit `MAYAP_SENSOR_PROFILE` (0=AUTO, 1/2/3 as above) replaces the split temperature/humidity flags, with a compile error for obsolete flags.
- **CI policy:** unit/integration, real E115, actual GPIO, format, pulse-energy/timing, and the SP30 anti-windup case are hard regression gates. The full uncalibrated plant grid remains an experimental CSV artifact and CI warning; failing physical-performance targets are not hidden or relaxed.

AUTO is a commissioning convenience, not a replacement for a verified module register map. AUTO temperature identification assumes a real startup temperature of 10–60°C. Raw-only identification cannot universally distinguish out-of-envelope values. Likewise, a native RH raw value of 600 is plausibly ~0.9% and cannot be distinguished from an RH-X10 encoding by magnitude alone once native profile is locked. Actual module register map and commissioning raw readings must establish its format. Native data order on I2C is not assumed to equal the module's RS485 register order; firmware retains the existing RH-then-T Modbus order. After lock, invalid T/RH yields Sensor Fault and heater OFF; plausible upward raw T reaches EmergencyHigh immediately. CRC retry/staleness behavior is unchanged. Main Web/HMI display stays at one decimal; service/internal paths retain native resolution.

## A. CONTROL-ONLY PERFORMANCE — NO PRODUCTION SAFETY INTERVENTION

[plant.csv](thermal-v2/plant.csv) has 1,536 deterministic, three-hour runs: 384 OLD fixed-10 s-window cases and 384 cases per NEW 300/500/1000 ms quantum. Each path sees identical uncalibrated lumped plant, noise, 2 s sensor cadence, resolution, median/IIR, ambient, delay and disturbances. The 100 ms integration step resolves all three quanta. OLD PID/window bodies are frozen from main `9569fcc` and SHA-256-guarded; this is a common-filter comparison, **not a full historical firmware binary replay**. Both switch one 16 kW bank. Plants have 0.18/0.6/1.6 MJ/K capacity, 120/180/300 W/K loss and an 8 s heater/air lag. Dead times: 5/15/30/60 s. Sensor resolution: 0.1/0.01°C. Scenarios include cold start, vent, door, ambient rise, noise, one bad sample, sensor loss/recovery, safety cut/resume and setpoint step. Final 30 min supply control-only debug metrics. Once High is crossed, subsequent MAE/ripple/settling values cannot describe normal production performance. The model intentionally continues integration for OLD/NEW algorithm comparisons.

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

OLD is still better at 35°C and 37.5°C mean MAE; NEW is not a universal thermal improvement. Original adversarial case SP30/ambient28/dead30/res0.01 had **2.547232°C NEW MAE before the anti-windup fix** in the prior 250 ms-step/1000 ms-quantum model. With only the PID fix and the same 250 ms-step/1000 ms quantum, MAE is **0.113949°C**. In the updated 100 ms sweep the candidate 300 ms case is **0.086605°C MAE**, +0.051566°C bias, 1.564% tail requested power; frozen OLD is 0.123133°C MAE. The 100 ms sweep is a separate timing experiment and should not be treated as the same binary run.

**Acceptance targets are still missed: 0/1536 runs pass all MAE, P95, ripple, overshoot and settling criteria.** The model is uncalibrated; modeled cold-start overshoot is especially high. CI retains this failure as an experimental warning. It cannot establish real chamber stability, sensor absolute accuracy or spatial uniformity.

## B. SAFETY THRESHOLD CROSSINGS

[safety-qualification.csv](thermal-v2/safety-qualification.csv) qualifies all 1,536 runs against stored default High=38.2°C and Emergency=39.0°C, independently of thermal targets. NEW300/SP37.5 crosses High in **168/168** runs and Emergency in **106/168**. Across all 384 runs per path, High/Emergency counts are OLD **189/118**, NEW300 **190/120**, NEW500 **190/120**, NEW1000 **190/120**. Each run records first crossing seconds (-1 means none), peak strictly before crossing, crossing flags, model scope and `SAFETY_INTERVENTION_REQUIRED` or `EMERGENCY_INTERVENTION_REQUIRED`. Plant temperature is checked at every 100 ms integration step; this is conservative threshold qualification, not replay of the sensor cadence/High confirmation timer, vent response or production safety state machine. No vent/safety physics or PID gains were added or tuned in this review. Existing injected vent/disturbance scenarios are unchanged.

300 ms remains an initial commissioning default. Its mean MAE differs from 500 ms by only ~0.0027°C in this uncalibrated model, with substantially more transitions; final quantum requires physical validation. **C. PHYSICAL PERFORMANCE: unverified.** CI green means software gates pass, not thermal acceptance or physical accuracy.

## Low-duty, timing and safety gates

[low-duty.csv](thermal-v2/low-duty.csv) includes 0.1/0.25/0.5/1/2/3/5/10/20/50/75/100%, each at 120/600/3600 s and 300/500/1000 ms. It reports requested/delivered average, absolute energy error, maximum no-heat interval and transitions/hour. A full GPIO1 pulse is 16 kW, so one 300/500/1000 ms packet is 4.8/8/16 kJ. At 0.5% over 120 s, 300 ms delivers 0.5%, 500 ms 0.4167%, 1000 ms 0%; all converge to 0.5% over 600/3600 s. At 1% over 3600 s all deliver 1%; maximum no-heat intervals are 29.7/49.5/99 s and transitions/hour 239/143/71. At 3600 s the maximum energy error over all requested duties is 0/1.6/9.6 kJ respectively. Finite-horizon error is bounded to one packet; there is no drift. Worst-case 100%-to-alternating switch patterns can reach ~12,000/7,200/3,600 transitions/hour. Actual scheduler jitter tests guarantee no accidental 20/50/100 ms catch-up pulse, no stale heat credit, and immediate safety OFF. Physical SSR transition/thermal endurance remains to be checked.

New production-cadence tests run the real scheduler and OutputArbiter and sum observed GPIO1 ON at 5 ms intervals. 5/10/30/50/100% requests reach 900,000 ms actual ON after 18,000/9,000/3,000/1,800/900 s respectively; E115 evidence equals the observed sum at every tick. Inhibit arriving after 135 ms is applied at 140 ms (next tick), clears scheduler credit and contributes exactly 140 ms ON, with no compensation after resume. A separate one-hour cadence pattern 5/5/5/10/5/20/.../40 ms checks pulse length, bounded error throughout the run, no catch-up, rollover and immediate safety OFF at all 300/500/1000 ms candidates.

[filter-compatibility.csv](thermal-v2/filter-compatibility.csv) compares frozen main `9569fcc` Q8 median-3/IIR-3/8 methods to actual float production methods over 60,000 identical X10 samples, including 375/375/376/375/374/375, ramps, steps and long tails. Max absolute delta **0.000781250°C**, mean absolute delta **0.000640725°C**, RMS **0.000703844°C**; hard bound 0.002°C. X10 float output is a **filtered estimate**, not a native 0.01°C measurement. Native X100/SHT30 resolution still survives decoding/filter/PID.

Legacy `pidCycleSec` is hidden from both HMI navigation and Web forms; internal protocol/readback and schema are retained. Tests load real byte-backed EEPROM records through actual CRC/migration/load paths for schemas 3–12, with values 1/10/60, and reject corrupted CRCs. Actual production heating waveforms remain identical at these legacy values; quantum stays 300 ms. HMI retains an inaccessible reserved array slot to preserve ventilation indexes. No EEPROM layout or migration algorithm changed.

E115/full-bank, 16 PID and 15 AutoTune actual heating-path safety cuts, native T+RH and X10/X100 AUTO locks, locked-format rejection, 3,000-cycle anti-windup, NaN/clamps, 1,000,000 variable-demand slots and millis wrap are tested. `maxHeaterPower=50` means 50% average of 16 kW. AutoTune's 30% request delivers 30% average through the same scheduler in the actual MachineController harness. Mechanical relay wear counters do not count pulse-rated SSR edges.

The config invariant remains `highTempAlarm >= targetTemp + HIGH_ALARM_GAP_C` and `emergencyTemp >= highTempAlarm + EMERGENCY_ABOVE_HIGH_C`. Final hardening also repairs one narrowly defined legacy case: if SV differs from 37.5°C while **all five** low/high/emergency/vent-on/vent-off thresholds are still exactly the 37.5°C defaults, the full envelope is shifted by the same SV delta before normal clamps. Any customized threshold disables that repair and is preserved. Schema13 layout is unchanged.

## Test/CI and reproduction

`python3 tools/test_thermal_control.py --sanitize --report-dir /tmp/mayap-thermal-report` runs all hard thermal tests and generates plant, low-duty, safety-qualification and filter-compatibility CSVs. `--require-targets` intentionally fails while uncalibrated performance targets are missed. Reliability CI runs the hard suite and uploads metrics, with a warning for experimental misses. All existing Node/account/protocol, actual runtime buses, EEPROM/notes, browser/workerd and firmware DEV/PROD builds must remain green before merge. No real ESP32 temperature-control test is claimed.

## AUTOTUNE QUALIFICATION

### ALGORITHM VERIFIED

The AutoTune qualification below retains its existing public state values, GPIO1 full 16 kW bank, PID defaults 18/0.8/45 with beta=1, 300 ms scheduler, RS485/filter cadence, safety policies and EEPROM schemas. The later Adaptive section separately documents schema13 and its opt-in setting.

Flow: START/preconditions → PREHEAT → fresh relay measurement → discard startup cycle → three repeatable cycles → Ku/Pu → Tyreus-Luyben → gain validation → existing atomic save. PREHEAT requests **30% average total bank power (4.8 kW)**, capped by `maxHeaterPower`; it does not request 100% or escalate. At SP minus band, peak/period measurement starts fresh. PREHEAT samples never contribute to Ku/Pu. Relay high is the configured relay power capped by `maxHeaterPower`, relay low is zero. All power goes through the existing scheduler and OutputArbiter; neither AutoTune nor PID writes GPIO.

Deadlines remain bounded: **PREHEAT 900 s; each HEATING/COOLING phase 900 s; total including PREHEAT 2700 s**. They are checked every control cycle, even without a new sensor sample. No extension was made to rescue slow plants. Numerical minimum amplitude/period, the discarded warmup cycle, three-cycle ±20% repeatability and rolling qualification remain. The relay is intentionally not biased: successful cycles show heat/cool ratios 0.222–2.37, but this matrix does not justify a new relay algorithm.

Terminal reasons are SAFETY_ABORT, SENSOR_ABORT, MODE_ABORT, PREHEAT_TIMEOUT, PHASE_TIMEOUT, TOTAL_TIMEOUT, INVALID_KU, INVALID_GAINS, SAVE_FAILED and SUCCESS. AMPLITUDE_TOO_SMALL, PERIOD_TOO_SMALL and NON_REPEATABLE are exposed as measurement rejection diagnostics while qualification continues within the original bounded deadline; if it expires, the terminal reason is the appropriate timeout and the rejection is retained separately. Serial prints only start, phase transitions, completed cycles, validation and final result. Diagnostics include actual relay high/low, extrema, amplitude, heat/cool duration, Pu, Ku, common gain scale and failure reason. No per-control-tick serial stream was added.

Ku uses `4 * ((actualHigh - actualLow) / 2) / (pi * amplitude)`, never PREHEAT power. Tyreus-Luyben coefficients 2.2/6.3 are unchanged. A common gain scale preserves Ti/Td; it is rounded upward one float ULP to avoid falsely rejecting a mathematically exact Kd=200 boundary as 200.000015. Finite, positive, range and sanitizer-preservation checks precede saving. RAM gains are assigned only after existing verified `saveConfig` succeeds. No EEPROM layout, migration or write algorithm changed.

Hard tests exercise max heater 20/30/50/100%, actual capped relay swing, invalid gains, wrap including an upper crossing at millis zero, phase/total/preheat timeouts, 34 safety/mode/sensor cuts across PREHEAT and relay, restart/reset and every partial ConfigRecord byte cut. Abort and save failure clear scheduler credit, turn heating OFF at the next control tick and retain old RAM gains. Partial records fail CRC and preserve the old valid EEPROM bank. As with the existing atomic store, a fully committed record followed by lost verification acknowledgement can leave a complete new bank on reboot; atomicity prevents mixed gains, rather than proving an acknowledged outcome after every possible power loss.

At that AutoTune milestone, outside-batch EventLog retained **Boot, AutoTuneStart and AutoTuneEnd only**; the later Adaptive milestone adds bounded adaptive transitions. Normal network/input/output events remain suppressed. This is the existing bounded RAM event log; no new persistent sink or high-rate EEPROM logging was added.

Final polish also records an operator cancellation as a distinct bounded `AutoTuneEnd/AutoTuneCancelled` terminal event (code 56), while retaining the previous PID gains, clearing heater credit and applying the existing post-cool/restart lockout. The Web success text explicitly says that saved gains still require a no-load physical confirmation; this is a UX/safety clarification, not a claim that the simulator certifies the chamber.

### SIMULATION VERIFIED — qualification, not physical performance

[autotune-plant.csv](thermal-v2/autotune-plant.csv) contains **864 actual plant-in-loop runs**; [autotune-cycles.csv](thermal-v2/autotune-cycles.csv) records each measured cycle. The plant capacities/losses are extracted from the existing model, with its same 8 s actuator lag: actual AutoTune → PDM/arbiter → 0/16 kW → plant → 5/15/30/60 s dead time → 2 s sensor poll → 0.1/0.01°C quantization → actual median-3 and IIR-3/8. No alternating PV was supplied to these plant tests.

648 COLD runs test PREHEAT 30/40/50%, three plants, ambient 20/25/28°C, four delays, two resolutions and relay 20/30/40%, SP37.5. Another 216 separately labeled NEAR_SP runs start the same physical model at 37.0°C using PREHEAT30 to qualify relay behavior where cold-start deadlines are insufficient. They are warm-start tests, not proof of cold-start tuning. Six additional real-loop disturbance cases cover invalid PV, confirmed sensor loss and safety inhibit, in cold/preheated conditions; all abort cleanly without saved gains. Existing bad-packet/staleness policies are unchanged.

| Cold PREHEAT candidate | SUCCESS / 216 | PREHEAT_TIMEOUT | SAFETY_ABORT | TOTAL_TIMEOUT | Model High / Emergency, including coast |
|---|---:|---:|---:|---:|---:|
| 30% | 25 | 156 | 35 | 0 | 31 / 6 |
| 40% | 20 | 144 | 52 | 0 | 48 / 18 |
| 50% | 29 | 120 | 63 | 4 | 60 / 24 |

**30% is the safety-oriented candidate**, not the fastest or the candidate with most successes. A stronger PREHEAT increases safety aborts and modeled residual overheating. Default PREHEAT30 qualification is:

| Relay power | COLD SUCCESS / FAIL (72) | NEAR_SP SUCCESS / FAIL (72) |
|---|---:|---:|
| 20% | 12 / 60 | 26 / 46 |
| 30% | 9 / 63 | 32 / 40 |
| 40% | 4 / 68 | 27 / 45 |

COLD: Light 25 successes, Medium/Heavy zero; 156 PREHEAT_TIMEOUT and 35 SAFETY_ABORT. NEAR_SP: Light 27, Medium 56, Heavy 2 successes; 48 SAFETY_ABORT, 32 TOTAL_TIMEOUT, 27 PHASE_TIMEOUT, 24 PREHEAT_TIMEOUT. Successful durations are 1182–1864 s cold and 626–2626 s near-SP. Across the 432 default candidate cases, 83 safety aborts save no gains. They include excessive lag/residual heat and insufficient margin from SP+band to the stored High alarm. Safety remains an abort, never a normal oscillation peak.

For Heavy/ambient20, loss at SP is `300 W/°C * (37.5-20) = 5.25 kW`, greater than 30% of 16 kW = 4.8 kW. That power is physically insufficient **in this model**; all eight COLD and eight NEAR_SP default30/relay30 cases end PREHEAT_TIMEOUT, retaining old gains. Other Medium/Heavy cold starts can have sufficient eventual power yet still exceed the 900 s PREHEAT deadline because of thermal inertia. This limitation is reported, not hidden by changing the model or deadline.

The loop checks actual AutoTune raw/filtered High/sensor guards and turns power OFF; after completion it also models 120 s passive coast. Vent cooling physics are not modeled. High/Emergency counts refer to model temperature, including residual heat after OFF, not a production alarm timing replay. None of the successful tuning runs crosses High/Emergency during tune or coast. An abort cannot instantly remove already stored thermal energy.

### Post-tune qualification

Every SUCCESS is atomically saved in the byte-backed real-store harness, then DEFAULT and TUNED PID are compared from identical cold ambient initial conditions for three hours. These follow-up runs are explicitly **CONTROL-ONLY POST VALIDATION / NO PRODUCTION SAFETY INTERVENTION**: crossing an alarm threshold means intervention would be required, not permission to operate the real machine through it. BAD means Emergency crossing, no settling or ripple above 1°C; the softer original performance targets remain reported and were not relaxed.

Representative NEAR_SP results (ambient / dead time / resolution / relay shown to avoid mixing conditions):

| Plant / condition | Kp / Ki / Kd | Common scale | TUNED MAE / P95 / ripple / overshoot °C | Qualification |
|---|---|---:|---|---|
| Light / 20°C / 5s / 0.1°C / 20% | 6.406779 / 0.014808 / 199.999985 | 2.586713 | 0.046612 / 0.047213 / 0.001630 / 2.160800 | BAD |
| Medium / 20°C / 5s / 0.1°C / 30% | 3.641618 / 0.004784 / 199.999985 | 7.954444 | 0.049453 / 0.051508 / 0.005893 / 3.700734 | BAD |
| Heavy / 28°C / 5s / 0.01°C / 40% | 2.617728 / 0.002472 / 199.999985 | 19.697268 | 0.324878 / 0.757232 / 0.939185 / 2.362475 | BAD |

Default PREHEAT30 yields 110 SUCCESS gain sets; **92/110 are BAD in cold-start post-validation, all with modeled Emergency crossing**. Mean steady MAE across them is DEFAULT 0.165214 versus TUNED 0.049169°C; this average does not cancel cold-start overshoot. Heavy's two near-SP successes are worse than DEFAULT: mean MAE 0.523371 versus 0.074237°C. Large common gain scales and small Ki can slow recovery. Do not treat a software AutoTune SUCCESS as physical gain acceptance. The original 1536-case comparison and its 0/1536 joint target passes remain unchanged.

### PHYSICAL UNVERIFIED / controlled commissioning

First tune must be outside a batch, with no live eggs, door closed, circulation fan in its intended state, sensor in the intended position and an independent reference probe. Begin with low 20–30% relay power; do not automatically raise it to rescue a timeout. Check raw/filtered temperature, sensor format lock, SSR temperatures, actual electrical power, High/Emergency margin, phase/cycle logs and post-OFF coast. A failed or aborted tune requires operator investigation before restarting. A successful tune requires supervised response/soak qualification before use with eggs.

Software hard gates and simulation failure behavior qualify the code for this supervised step only: **AUTOTUNE READY FOR CONTROLLED PHYSICAL COMMISSIONING.** No hardware tune, merge, deploy or OTA was performed.

## Adaptive Thermal Balance / Tự cân bằng nhiệt

### Architecture and scope

This feature is opt-in, **default OFF**, layered above the existing PID. GPIO1 still switches both SSRs together as one 16 kW bank; the latest user hardware description has eight elements, four on each side of the central fan. There is one existing paired-profile RS485 probe, not a sensor for each of the two compartments. No new GPIO, automatic setpoint change, PID gain scheduling, automatic AutoTune, filter/poll change or new safety authority was introduced.

`ThermalObserver` is read-only; `AdaptiveThermalSupervisor` chooses a bounded actuator ceiling and a separate ON/OFF cooling request. MachineController supplies actual GPIO state/context, a transient config view passes the effective integer-percent ceiling into the unchanged PID anti-windup, then the unchanged 300 ms PDM/OutputArbiter drive GPIO1. Between samples the requested power is bounded by that same ceiling; it is not a second control law. `runtime.heaterPower` remains requested average total-bank power, never instantaneous ON/OFF. Ceiling rounding is conservative (floor to the existing uint8 percent representation).

### Observer equations and validity

- Delivered energy: `16000 W * actual GPIO1 ON milliseconds / 1000`, integrated in O(1) each control cycle. Requested duty is never substituted for delivered energy. A >1 s tracking gap invalidates the window; no unknown historical energy is credited.
- LoadIndex: apparent inverse response `1 / (1 + ΔT_C / delivered_MJ)`, bounded 0..1, low=fast/light response, high=slow/heavy response. Windows are 120 s, require at least 160 kJ and 0.1°C rise below the holding band; estimates blend 3/4 old + 1/4 new. This is not independently identified heat capacity: loss, ambient and internal heat can affect it. **It does not identify cart count.**
- CoastRise: following an episode with ≥10 s actual full-bank ON, use filtered PV at final OFF and maximum PV during ≤180 s uninterrupted OFF. Interrupted PDM OFF gaps do not qualify. Median of last five accepted rises, with outlier rejection; smoothed time-to-peak supplies a lag index. It is not a full physical decay model.
- HoldPower: actual delivered ON duty in a complete 120 s window, only if every sample stays within ±0.15°C of SP and |rate|≤0.001°C/s. No requested-output estimate.
- Rate: bounded least-squares fit over 64 sensor samples (126 s at the unchanged 2 s poll), followed by smoothing. A 30 s trial failed the real-filter X10 slow-self-heating regression; 126 s passes both 0.1/0.01°C formats. Sensor/PID median, IIR and derivative filtering are unchanged.

Learning is forbidden during invalid/non-finite sensor data, unstable circulation command/actual relay, AutoTune, safety/inhibit, ventilation, own cooling, startup/recovery, maintenance/test, raw/filtered PV within 0.3°C of High, a >0.35°C sample jump or >10 s sensor sample gap. Disturbances require 180 s stable re-entry. Confidence/validity suspension also happens on a control tick without a new sensor sample and on early Test/Resume output returns. Failed samples never become permanent model evidence. Slow ambient/internal-heat changes cannot be uniquely separated with one probe; only net thermal response is estimated.

### Confidence, state machine and authority

States: Disabled → Learning → Qualified/Adaptive; invalid evidence → Degraded; sensor/safety/recovery/AutoTune/test/maintenance → FaultBypass; separately qualified positive heat → SelfHeating. All transitions and times are deterministic and wrap-safe.

Thermal confidence adds 8 per consistent load window, 12 per hold/coast window, saturating at 100. Inconsistent load/hold or disturbances halve it; critical sensor/safety evidence sets it to zero. Eligibility needs ≥50 confidence, ≥600 s continuously valid learning and **five fresh windows after the last invalidation**. ≥80 plus at least two coast observations allows the wider envelope. These are software commissioning bounds checked by hard tests, not fitted physical chamber constants.

LOW/unqualified evidence chooses baseline configured authority. MEDIUM may reduce the ceiling by at most 20%, with a floor `min(configuredMax, max(10, 1.5*holdPower+10))`. HIGH selects `configuredMax*(0.5+0.5*LoadIndex)`, respecting the same floor. A proven ceiling returns gradually to baseline after ordinary degradation so changed load cannot cause an authority jump; this is rollback, not a fresh low-confidence estimate. Explicit OFF, internal invariant failure, safety/mode/AutoTune bypass and user configuration limits take precedence. There is no authority above configured max. Normal increases are ≤5 percentage points/minute, reductions ≤20/minute; protective self-heating cuts to zero are immediate. Both normal slew and exceptional bypass changes are reported separately.

Soft landing changes only this ceiling. Approach band is `coastRise + max(rate,0)*timeToPeak`, clamped 0.15..1.0°C. Within the band, positive rate progressively reduces the ceiling, while preserving the holding floor. No SP or gains change. Any PID/SP/offset/max/control-mode/sensor-profile compatibility change invalidates current qualification; previous estimates are low-trust history until fresh windows requalify.

### Self-heating and cooling

The self-heating channel needs valid, startup-qualified samples, actual heater OFF ≥180 s, PV>SP+0.12°C and fitted positive rate >0.0003°C/s maintained for 60 s. This is a separately confirmed protective channel; no single sample or persisted self-heating flag can authorize it. Runtime confidence now always reports thermal-observer/model confidence; SelfHeating state/boolean reports the independent protective confirmation instead of manufacturing an 80% model-confidence value. Heating authority becomes zero. Exit at PV≤SP+0.04°C; after cooling releases, the pre-intervention bounded authority ceiling is restored (clamped by the current configured maximum) so a protective cut cannot leave the chamber crawling upward from 0% at 5 percentage points/minute. Fresh evidence still has to requalify subsequent adaptation.

“SelfHeating” means sustained **net positive heat after the bounded coast interval**, not proof of embryo watts. Late physical coast, ambient change or sensor drift could still mimic it. It does not identify which compartment/cart produces heat.

CoolingDemand is staged 0/100 as a request for the existing ON/OFF exhaust relay, not fan speed or proportional cooling power. Adaptive minimum ON/OFF is 120 s, compile-time locked to the existing normal vent-relay minimum ON/OFF timers. Safety High/Emergency/fault fan forcing is ORed independently and always wins; smart cooling cannot cancel it. Scheduled/profile ventilation and RH rules remain intact; their actual cooling periods are excluded from learning. OFF removes the adaptive request immediately; actual relay release still observes existing non-safety minimum timings.

### OFF rollback, AutoTune and diagnostics

A frozen b537786 heating-body oracle compares the actual new heating path at SP30/32/35/37.5 over 288,000 five-ms ticks: Adaptive OFF GPIO waveform, requested power and vent output match exactly. OFF resets observer/supervisor/self-heating, discards pending model saves and clears scheduler heat credit on the toggle; ON again starts learning. AutoTune bypasses all adaptive actuation and learning; its existing 864-case qualification still passes unchanged. New gains invalidate the model signature; tune failure retains historical estimates but cannot preserve active qualification through the disturbance.

Web/HMI expose the same ON/OFF setting; old firmware has the Web toggle disabled and no unsupported patch key is sent. Runtime diagnostics include state/reason, confidence, load, coast/time-to-peak, hold power, effective ceiling, approach, self-heating, cooling and valid-window count. Serial `[THERMAL-ADAPT]` is bounded to 30 s; state/enable/disable/model invalidation use the bounded EventLog. Outside batches the earlier Boot/AutoTune exception is extended only for Adaptive transition events; ordinary network/I/O remain suppressed. No new persistent event sink was added.

### Config and learned-model persistence

Config schema13 appends one byte. Real schemas3..12 are CRC-checked and migrated with the new feature OFF; slot addresses, reminder/history/notes regions and atomic config save remain unchanged. Existing schema12 payload size/CRC have their own exact legacy record. New ON/OFF roundtrips through the existing signed config/verified EEPROM transaction.

Learned models use a **separate `mayap_thermal` NVS namespace**, two alternating versioned/CRC records with sequence, RTC epoch, thermal/hardware/profile compatibility signature and scalar estimates. EEPROM machine config remains in AT24C512. No NVS/EEPROM/file/network I/O is done by the observer or adaptive control hot path. A fixed mailbox hands qualified snapshots to the existing Arduino loop; no new task/WDT architecture. Writes are at most hourly, including failed-attempt backoff, and only snapshots whose supervisor state is actually Qualified/Adaptive are offered (preventing pre-requalification history from being stamped under a new compatibility signature; a stable unchanged snapshot may refresh hourly); invalid/OFF/test/recovery cancels pending offers. NVS failure is optional-feature failure, never a machine boot/storage/safety failure.

Restore requires valid CRC/version/ranges, compatible PID/SP/offset/max/control mode/sensor profile/bank/GPIO/quantum, RTC age≤7 days and no abnormal-reset latch. Restored confidence is10, window counts/state/heat credit remain zero: a seed cannot enable adaptive actuation. It is consumed only when enabled with valid sensor/RTC. All model bytes are CRC-corruption tested; all partial-record write cuts preserve the last valid record. This does not guarantee acknowledgement after a fully committed write followed by reset.

### Honest simulation and hard qualification

[adaptive-summary.csv](thermal-v2/adaptive-summary.csv): 1248 runs / 624 identical-condition baseline/adaptive pairs, each3h. Same Light/Medium/Heavy capacities/losses and 8 s heater lag as the existing model; ambient20/25/28, delays5/15/30/60, resolution0.1/0.01, SP30/32/35/37.5. Disturbance subset at ambient25/SP37.5 covers light→heavy, heavy→light, loss up/down, ambient rise, door drop, internal heat0→500→1000→2000 W, invalid PV, sensor loss/reconnect, inhibit, scheduled vent, SP step, power recovery and Test Mode. All inputs pass through actual filter, PID, supervisor, PDM, arbiter and heating orchestration. No cart-number or spatial-temperature labels are assigned to plant classes.

[adaptive-observer.csv](thermal-v2/adaptive-observer.csv) and [adaptive-control.csv](thermal-v2/adaptive-control.csv) retain representative minute traces (16,200 rows each). Cooling capacity is physically unknown: the simulator credits **zero cooling watts**, records the actual exhaust request/runtime and does not fake vent heat removal. Physical threshold flags use conservative instantaneous plant crossings; production alarm confirmation timers/vent physics are not replayed. Internal-heating watts are stimulus assumptions, not embryo measurements.

Performance remains experimental. Across624 pairs: MAE better100/same450/worse74; P95 better105/same450/worse69; ripple better102/same450/worse72; overshoot better159/same455/worse10; heater energy lower133/same450/higher41. Mean steady MAE baseline0.323809 versus adaptive0.323257°C. High crossings430 versus328; Emergency158 versus158. Adaptive authority is active in361 runs; self-heating is not confirmed in any plant-matrix run after the final valid-learning gate. Net-heating stimuli can be below heat loss/rate threshold or hit safety/vent inhibition before the conservative OFF/confirmation period. Dedicated slow-rise tests through the real quantized sensor filter pass, but this matrix does not establish successful plant-in-loop self-heating detection or useful cooling; that limitation remains a physical commissioning risk. **Neither mode meets all absolute thermal targets in this matrix**; the original0/1536 target result also remains unchanged. Default gains are not tuned to make this feature win.

Hard gates include the frozen OFF oracle, nine actual-path safety cuts, eleven prohibited-learning contexts, immediately invalid sensor without a new sample, confidence/requalification, X10/X100 self-heating/noise, read-only commissioning, coast/hold/energy windows, normal slew, dynamic PID saturation/no stale integral, cooling minimum timings, OFF/ON/reset, wrap, CRC/age/signature/all-byte model write cuts, real config migrations and existing E115/Frozen accounting. Plant gates reject false learning, new Emergency crossings, newly lost settling and new gross sustained ripple in previously settled cases; performance targets were not relaxed. Host CPU timing is checked against the extracted existing period/trip budget; it is a proxy, not proof of ESP32 task jitter under flash/network/ISR load.

### Controlled physical commissioning

Default remains OFF. First build with `MAYAP_ADAPTIVE_OBSERVER_ONLY=1` (local build override), enable only observer diagnostics, compare delivered energy, load/coast/hold/rate and confidence to independent probes. Then manually use the normal build (`0`) and enable the setting after review. No batch/live eggs in the first commissioning; circulation as intended, doors closed, sensor placement verified, independent calibrated probes in both compartments and several cart positions. Check SSR/contactors/current/heatsinks, fan relay minimum timing, raw/filtered slope, coast after OFF, model restore/invalidation, OFF rollback, safety cuts and actual controlMaxCycleUs under long offline/online soak. One probe cannot establish hotspot/airflow balance or ±0.1°C accuracy. No deploy, OTA or physical flash was performed during software qualification.

**ADAPTIVE THERMAL BALANCE — SOFTWARE READY FOR CONTROLLED PHYSICAL COMMISSIONING.** This marks software qualification only; physical actuation and thermal performance remain unverified.

## REMAINING RISKS

- Confirm GPIO1 drives both SSR inputs on the physical board as specified, their zero-cross behavior, rating, heatsinking, contactor and 16 kW electrical loading. High burst transition rates make this a commissioning blocker until measured.
- Confirm the RS485 module register map and six-sample paired AUTO lock at known T/RH. Raw-only AUTO is not a universal profile detector outside the stated envelope; overlapping plausible raw values cannot be rejected reliably.
- Current default/stored PID gains are uncharacterized on the 12 m³ chamber. The uncalibrated model misses all joint performance targets and NEW is slightly worse than OLD in some 35/37.5°C cases. Run safe physical response identification/AutoTune before accepting thermal performance.
- Legacy configs whose entire thermal envelope was still the untouched 37.5°C default are now re-anchored with SV. Customized envelopes are deliberately preserved, so operators must still review intentional/custom safety thresholds before low-temperature operation.
- A single probe's stable reading does not prove ±0.1°C absolute accuracy or spatial uniformity throughout the chamber. Independent calibrated probes and a long soak are needed.
- 300 ms is provisional; verify real control task jitter and SSR temperature over a long soak before accepting final actuator timing.

- AutoTune PREHEAT30 can be insufficient or exceed the fixed deadline; High/Emergency residual coast and 92/110 BAD cold-start post-validations prevent unattended acceptance. Commissioning must validate power, margin, generated gains and recovery on the real chamber.

- Adaptive LoadIndex is apparent net response, not cart count or spatial uniformity; one probe cannot identify compartment temperatures/hotspots/airflow imbalance.
- Real chamber gains, cooling capacity, SSR endurance, model staleness, long coast and 0.1°C slow-trend discrimination require physical qualification. Persistent restore is deliberately low-trust.
- No measured ±0.1°C accuracy or full control-task timing under real flash/network/ISR load is claimed. All new bounds are software commissioning candidates.

### Final local software gates

- 160 Node/account/protocol/realtime tests PASS; native Chromium Web/connection/notes and real workerd suites PASS; web assets built.
- Full ASan/UBSan thermal suite retains the 1536-row historical matrix and 864 AutoTune runs, plus 1248 Adaptive runs. New hard coverage includes 288,000 OFF-oracle ticks, nine actual-path safety scenarios, eleven forbidden-learning contexts, 48 per-byte single-bit model CRC corruptions and 48 partial-write cuts, both sensor resolutions, slew/anti-windup/wrap/read-only and schema3..13 migration with ON/OFF/reboot roundtrips. These are coverage counts, not a claim that each assertion is a separate test.
- Actual runtime buses/transactions, AT24C512 notes/store/power cuts and boot/recovery suites PASS. Preservation hashes retain PID, AutoTune, alarms, arbiter and PDM bodies; integration/new-model hashes are reviewed explicitly.
- ESP32-S3 Arduino3.3.11/IDF5.5.5 DEV: 1,412,409 bytes flash / 199,440 static RAM; PROD: 1,388,489 / 197,544. Both linked ISR IRAM/DRAM checks PASS. Host route timing is a budget proxy; physical task jitter remains unverified.
- Exact final-commit GitHub CI is reported on the draft PR and in the completion report. No software performance target was relaxed to obtain a green hard-regression result.

Remote commits `013d931` / `f9fe684` arrived during qualification and were retained by rebasing the adaptive commits: setpoint/unrelated config saves preserve the P response; gain/cap changes remain bumpless at the prior SP. Their dedicated regressions and reviewed thermal-control hash are retained. Adaptive still does not tune PID gains; the frozen heating-body OFF oracle remains applicable because these remote commits do not change that body.
