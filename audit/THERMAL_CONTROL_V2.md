# Thermal control V2 — review / commissioning report

Branch: `codex/thermal-control-v2`, based on latest `main` at start of work: `9569fcc`.
No merge, OTA, hardware flash or Cloudflare deployment performed.

**Status: functional regression/builds pass; thermal acceptance targets FAIL with current default gains in the uncalibrated models. This is a review branch, not evidence that the machine meets ±0.1°C.**

## Hardware audit and production fallback

`config.h` proves only `PIN_OUT_HEATER_SSR = 1`. Other outputs are circulation 21, heat contactor 14, light 12, vent 13, turning 11/10, siren 47, spare/humidifier 48. No second heater GPIO or verifiable schematic is present. No B GPIO was chosen.

Production remains single-output on GPIO 1. Dual mode is compiled only if `MAYAP_HEATER_SSR_B_PIN` is explicitly defined after board/wiring verification. The existing pin uniqueness/range assertion includes B. Host tests use a deliberately synthetic HAL index outside the ESP32 range; that value cannot pass production pin validation and is not a suggested pin.

Confirm whether the existing SSR channel drives the entire wired heater bank or one 8 kW group. In single mode 0–100% refers to the existing configured bank; the simulation assumes a 16 kW bank. A/B semantics represent equal 8 kW groups only when two independent outputs and equal installed powers are confirmed.

Runtime flow:

```text
RS485 RH/T registers → raw format verification/lock → median 3 + IIR 3/8
                    → sample-driven PI-D → requested total average %
                    → HeaterBurstScheduler → desired A/B → OutputArbiter → GPIO
```

The two algorithms do not write GPIO. Runtime heater GPIO writes remain in OutputArbiter; the existing early boot/supervisor fail-safe output initialization also initializes optional B OFF.

## PID, anti-windup and autotune

P is `Kp*(beta*SP-PV)`, I integrates full `SP-PV`, D stays on filtered PV slope. Actual dt, sample-only updates, D filter, clamping and conditional anti-windup remain. For beta<1, I's finite bound includes the absolute-Celsius P offset; the old +/-maxOut bound alone can make the setpoint unreachable. I starts at zero; configuration changes still track the present output for bumpless transfer.

Selected production beta is **1.0**. The sweep does not justify a change: beta 1 has the lowest overall single-output MAE and lowest worst overshoot. Its controller outputs match the frozen OLD implementation over 200,000 samples, including disabled intervals, gain/output-limit changes and millis wrap. Gains already stored on the machine are untouched; defaults remain Kp=18, Ki=0.8, Kd=45.

PID integration is reset while heater permission is unavailable, including contactor pickup, fan interlock, sensor/fault/storage gates, boot, maintenance and system trip. Normal PDM OFF intervals do not reset I. End-to-end tests execute the actual controller heating method and actual GPIO arbiter for 16 PID permit cuts and 15 autotune cuts, both single and dual.

RelayAutoTune's body is unchanged and separately fingerprinted against `9569fcc`: Tyreus-Luyben, warmup discard, three validated stable cycles, timeout, safe abort and config save remain. Its requested power goes through the same scheduler: 30% means 30% total bank power, not a direct SSR bypass.

## Burst scheduling vs fixed window

The production 10 s SSR window and minimum-pulse clipping are removed. `pidCycleSec` stays in the existing EEPROM/HMI/Web schema for compatibility; it no longer controls heater bursts. The new production quantum is 1000 ms.

A bounded, fixed-point energy-error accumulator accounts for actual elapsed time. Quantum boundaries rebase on the current control cycle, so a late poll cannot create a 1 ms catch-up pulse. At <=50% dual power only one group is selected per quantum. The group with less accumulated ON time leads, balancing actual runtime even with poll jitter. Above 50%, both groups can run. Long stalls discard uncertain heat credit; permission loss, zero or nonfinite demand turns both groups OFF immediately and clears credit.

HeaterNotHeating/SensorFrozen retain their thresholds; dual ON time is measured as equivalent full-bank energy. One 8 kW group running for one second contributes 500 ms at total 16 kW. Half-ms remainders are retained. Mechanical relay wear alarms keep the same threshold; pulse-rated SSR transitions are excluded from that counter so expected burst activity cannot raise a false mechanical relay alarm. SSR events stay out of the HMI history, as before.

Low-duty results are in [low-duty.csv](thermal-v2/low-duty.csv). The constant-duty finite-horizon error stays below one group-quantum, and A/B quantum counts differ by at most one. Input quantization is <=0.00000005 percentage point. With actual poll jitter, energy error stays within one quantum plus the late-poll bound, and A/B runtime differs by at most one burst plus the poll bound.

Examples for single output:

| Requested | 120 s delivered | 600 s delivered | 3600 s delivered | OLD 10 s window |
|---:|---:|---:|---:|---|
| 0.5% | 0% | 0.5% | 0.5% | clipped to 0% |
| 1% | 0.8333% | 1% | 1% | clipped to 0% |
| 2% | 1.6667% | 2% | 2% | clipped to 0% |
| 3% | 2.5% | 3% | 3% | minimum 300 ms pulse |
| 5% | 5% | 5% | 5% | 500 ms pulse |
| 10% | 10% | 10% | 10% | 1000 ms pulse |

At 120 s, 0.5% requires only 0.6 whole single-output quanta, so zero delivery is the expected quantization result, not a claim of exact finite-horizon delivery. Error converges without accumulated drift. Safety intentionally discards pre-cut energy; it is never repaid on resume.

`runtime.heaterPower` remains the total requested average 0–100%, including when the current quantum is OFF. Aggregate SSR state is `A || B`, preserving cooling, output-state and alarm callers.

## Sensor formats and limits of raw-only detection

Temperature decoding supports signed TEMP_X10, signed TEMP_X100 and unsigned SHT30_RAW16. Native conversion matches [Sensirion's official Arduino implementation](https://github.com/Sensirion/arduino-i2c-sht3x/blob/e3a55be28ac6fcf357645eab1bd5f9485254fd6c/src/SensirionI2cSht3x.cpp#L47): `-45 + 175*raw/65535`. Manufacturer source was checked during this audit; direct datasheet download was blocked by the cloud network proxy.

AUTO locks only after six consecutive CRC-valid polls identify the same unique candidate. Heater permission stays unavailable throughout verification and the existing sensor recovery gate. Detection uses raw register magnitude and declared plausibility limits, never decimal digits. It never changes scale after lock, including UART recovery. Wrong range, a >20°C raw jump or a bad CRC makes the sample unusable; the existing Sensor Fault path cuts heater. Valid data can recover using the same locked format and existing controller recovery gate.

**Raw-only universal autodetection is impossible.** For example raw 500 can be 50°C X10 or 5°C X100; a stale value can also be valid under a wrong format. AUTO therefore has an explicit commissioning assumption: actual startup temperature **10–60°C**. In this envelope the three raw bands are disjoint. Outside this envelope AUTO is not a trustworthy universal detector: it may reject the sample or select an in-envelope interpretation of an out-of-envelope physical value. Use a verified explicit profile and still verify six samples on every boot. No EEPROM schema/cache was added.

Build overrides (do not supply unverified settings):

- `MAYAP_SENSOR_TEMP_FORMAT`: 0=AUTO (default), 1=X10, 2=X100, 3=native RAW16.
- Explicit formats use the existing -40..60°C operating plausibility range.
- `MAYAP_SENSOR_HUMIDITY_RAW16`: default 0 preserves the proven RH x10 Modbus register. Set 1 only if the module's register map proves native humidity ticks.

The existing Modbus register order stays RH then temperature. Native SHT3x I2C data order is different; firmware must not guess a new RS485 register map or humidity scale. Native temperature support is tested, but the physical module variant/map still requires verification.

X10 test: raw 375 -> 37.5°C. X100 test: raw 3751 -> 37.51°C, retained through the real median/IIR filter. RAW16 test: raw 30902 -> ~37.52°C. Calibration, filter and PID retain float precision. Poll remains 2 s; median remains 3; IIR remains 3/8. Only numeric representation changes from the old fixed tenths/Q8 filter to floats to retain the decoded resolution. Web/HMI code and one-decimal main display are unchanged. Service logs show format, raw register and raw/filtered temperature with extra precision.

## Thermal experiments — OLD and NEW beside each other

Full measurements: [plant.csv](thermal-v2/plant.csv), 3,456 deterministic runs. Each run lasts 3 h; steady metrics use the final 30 min. OLD PID/window bodies are frozen from `9569fcc` and guarded by SHA-256. Both algorithms receive identical gains, plant, noise, quantization and production filter methods. No model coefficients or acceptance thresholds were tuned to favor NEW.

Grid: SP 30/32/35/37.5°C, ambient 20/25/28°C, dead time 5/15/30/60 s, resolution 0.1/0.01°C. Three plausible but **uncalibrated** lumped plants use heat capacities 0.18/0.6/1.6 MJ/K and losses 120/180/300 W/K, with 16 kW rated power and an 8 s heater/air lag. The grid gives 288 cold-start cases per path; each also has 96 disturbance/step cases. Disturbances cover vent, door loss, ambient rise, deterministic noise, one bad sample, sensor loss/recovery and safety cut/resume.

The performance model isolates the control loop: it does not duplicate automatic firmware alarms, emergency limits, spatial gradients or sensor absolute offset. Thus large modeled overshoots are controller-performance findings, not predictions that the physical safety system would allow those temperatures. Safety correctness is checked separately using the actual controller/arbiter code.

Mean error below is **mean absolute error**, not a signed bias that can cancel oscillation. All temperatures/errors are °C. Settling means staying within ±0.15°C through at least the final 30 min; -1 means no settling. P95 is absolute error. Complete CSV also includes signed bias, max error and delivered total/tail power.
| SP | Path | Mean MAE | Worst MAE | Worst P95 | Worst ripple | Worst overshoot | Steady criteria | All criteria |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 30.0 | OLD window | 0.1838 | 2.2308 | 2.7185 | 3.5288 | 7.2166 | 30/72 | 0/72 |
| 30.0 | NEW single | 0.1748 | 2.5472 | 2.6188 | 3.5868 | 7.1743 | 34/72 | 0/72 |
| 30.0 | NEW dual | 0.2020 | 2.5307 | 2.6605 | 3.3001 | 7.1719 | 35/72 | 0/72 |
| 32.0 | OLD window | 0.1991 | 1.5260 | 3.3647 | 4.2243 | 6.9397 | 24/72 | 0/72 |
| 32.0 | NEW single | 0.1919 | 1.5498 | 3.4036 | 4.2964 | 6.9445 | 25/72 | 0/72 |
| 32.0 | NEW dual | 0.1874 | 1.4261 | 3.3391 | 4.1904 | 6.9062 | 26/72 | 0/72 |
| 35.0 | OLD window | 0.2653 | 1.6114 | 4.0138 | 5.1050 | 6.8959 | 18/72 | 0/72 |
| 35.0 | NEW single | 0.2654 | 1.8283 | 4.0796 | 5.0995 | 6.7325 | 18/72 | 0/72 |
| 35.0 | NEW dual | 0.2641 | 1.7727 | 4.0100 | 5.0810 | 6.7310 | 18/72 | 0/72 |
| 37.5 | OLD window | 0.3249 | 1.8444 | 4.6492 | 5.9000 | 6.6405 | 18/72 | 0/72 |
| 37.5 | NEW single | 0.3179 | 1.8571 | 4.5146 | 5.7862 | 6.5525 | 18/72 | 0/72 |
| 37.5 | NEW dual | 0.3260 | 2.1390 | 4.7609 | 5.8948 | 6.5514 | 18/72 | 0/72 |
Steady criteria are MAE<=0.10, P95<=0.15 and ripple<=0.25. All criteria additionally require overshoot<=0.30 and settling. **No cold-start case passes all criteria with these defaults.** Across all algorithm/beta/disturbance runs, 0/3456 pass all criteria. Functional tests passing must not be confused with this failed performance acceptance.

Beta sweep, NEW single output, all 384 cold/step/disturbance cases:

| Beta | Mean overshoot | Worst overshoot | Mean steady MAE | All criteria |
|---:|---:|---:|---:|---:|
| 1.00 | 2.4401 | 7.1743 | 0.2904 | 0/384 |
| 0.70 | 2.4372 | 7.2514 | 0.2912 | 0/384 |
| 0.50 | 2.4436 | 7.2496 | 0.2919 | 0/384 |
| 0.35 | 2.4383 | 7.2517 | 0.2998 | 0/384 |
OLD is materially better in some cases. For the light plant, SP30/ambient28/dead30 s/0.01°C resolution, steady MAE is OLD **0.127578** vs NEW single **2.547232**. NEW is materially better in the light SP30/ambient28/dead15 s/0.1°C case: OLD **2.230801** vs NEW **0.042349**. The large case-to-case variation exposes gain/delay sensitivity; PDM alone does not fix underdamped tuning. A/B balance is not proof of thermal stability or spatial uniformity.

The existing stored gains on the real machine, installed thermal mass/loss, fan mixing and actual dead time are unknown. Changing gains to fit an assumed model would not justify a production setting. Re-measure/retune on the machine before accepting temperature performance; preserve Tyreus-Luyben and all existing safety limits.

## Tests / CI and reproduction

Local verification:

- 158 existing Node tests PASS, including account/security/protocol/realtime and preservation gates.
- Reliability/release/ATtiny/history checks PASS.
- EEPROM/notes power-cut and regression-injection tests PASS with sanitizers.
- Actual UART/I2C/network/Tiny/OTA/WebSocket/TLS/transaction/notes-link runtime tests PASS with sanitizers and negative regression proofs.
- Existing thermal/autotune tests and new raw format, 2-DOF, anti-windup, bumpless, low-duty, actual GPIO/controller cuts, jitter, variable demand and millis-wrap tests PASS.
- Chromium connection/experience/notes tests and actual workerd/browser round trips PASS.
- Adaptive boot/runtime recovery tests PASS.
- ESP32-S3 Arduino 3.3.11 / IDF 5.5.5 DEV and PROD builds PASS, PSRAM disabled. DEV: 1,399,537 bytes flash, 198,184 bytes static RAM; PROD: 1,377,105 bytes flash, 196,288 bytes static RAM. Tiny/encoder handlers/callees/shared data pass linked IRAM/DRAM audit.
- Web assets staging and Worker bundle **dry-run** PASS; no deployment performed.

Reliability CI now runs the thermal driver and uploads OLD/NEW metrics as an artifact. Missing model targets are printed explicitly and surfaced as a CI warning. Regression correctness is a distinct gate from the thermal experiment targets. To enforce all experiment targets as a failing gate, use `--require-targets`; thresholds are unchanged and that gate currently fails.

```bash
python3 tools/test_thermal_control.py --sanitize --report-dir /tmp/mayap-thermal-report
python3 tools/test_thermal_control.py --sanitize --require-targets --report-dir /tmp/mayap-thermal-targets
```

## Changed files

Production: `config.h`, `thermal_control.h`, `machine_control.h`, new `sensor_format.h`, new `heater_burst_scheduler.h` under `MAYAP_INDUSTRIAL_v1_0_0/`.

Tests: `tests/thermal-control.cpp`, `thermal-fixture.h`, `thermal-v2.cpp`, `thermal-output.cpp`, `thermal-heating.cpp`, `thermal-plant.cpp`, `runtime-buses.cpp`, `runtime-preservation.json`, and frozen OLD `tests/fixtures/thermal-v1/{thermal_control.h,ssr_window.h}`.

Tool/CI: `tools/test_thermal_control.py`, `.github/workflows/reliability-checks.yml`. Review artifacts: this report and `audit/thermal-v2/{plant.csv,low-duty.csv}`.

Preservation hashes were updated only for the explicit thermal/sensor/output/config scope. Additional original hashes now protect RelayAutoTune, batch start/stop/time/save, tuning admission/update and turning. FaultManager, safety thresholds/timers/journal, EEPROM layouts, HMI/Web/Cloudflare, signed protocol, ATtiny, boot, watchdog, recovery and OTA implementations remain protected.

## Physical commissioning checklist

1. Review branch/PR before merging. Confirm GPIO1 load, SSR zero-cross suitability/rating, contactor protection and separate A/B wiring. Do not define B until a schematic/measurement proves its GPIO and output polarity.
2. With heater supply isolated, verify both SSR outputs OFF during boot, pending sensor lock, sensor disconnect/CRC fault, physical heater-disable, Emergency/High, fan interlock, journal/storage faults, maintenance/OTA and system trip.
3. Read the actual module model/register map and raw RH/T at known temperatures. Verify AUTO's boot envelope or set a proved explicit temperature/RH profile. Check six-sample lock, 37.51°C retention only if the module truly supplies it, and no scale change on disconnect/recovery.
4. Observe SSR signals for 0.5/1/2/3/5/10/25/50/75/100% with a scope/logger. Confirm no short catch-up pulses, A/B runtime balance, no two-group quantum at <=50%, immediate safety OFF and no stored burst on resume. Check actual zero-cross conduction and electrical/thermal load distribution.
5. Measure real plant response and stored PID gains at reduced commissioning power before tuning. Verify RelayAutoTune has enough permitted average power to reach SP, starts near a suitable temperature and safely aborts on timeout/fault. Keep its existing 15 min phase /45 min total limits.
6. Test 30/32/35/37.5°C across expected ambient/load, then fan/door/sensor/network interruptions. Log raw/filtered PV, requested power, actual A/B timing, overshoot/settling/MAE/P95/ripple. Run a prolonged soak. Use calibrated independent probes at several chamber locations.

## REMAINING RISKS

- Physical SSR B GPIO, independent group wiring, actual single-channel installed power and SSR zero-cross/rating are unproved.
- The RS485 module scale, register order and RH scale are unproved for alternative profiles. AUTO cannot universally distinguish formats outside its declared boot envelope, or detect every plausible wrong-scale value after lock.
- Current default gains fail the simulation acceptance targets; one important low-temperature case is materially worse with PDM. Real stored gains and plant coefficients must be measured before selecting a production tune.
- The model is lumped and uncalibrated, and omits automatic firmware alarms, absolute sensor error, fast local hot spots and spatial gradients. Chamber air alone has much less heat capacity than the modeled loaded plants.
- 1 s SSR bursts can still cause excessive local ripple with very low effective thermal mass. Cloud tests cannot prove mains zero-cross timing, EMC, SSR heating, electrical loading or control-task timing under real faults.
- Sensor resolution, absolute accuracy and spatial uniformity are separate. Stable PV at one probe does not prove uniform temperature across 12 m³ or ±0.1°C physical accuracy.
- HMI/Web still retain the legacy `pidCycleSec` field for compatibility; it no longer changes burst timing. That UI meaning needs a separate reviewed follow-up if desired.
