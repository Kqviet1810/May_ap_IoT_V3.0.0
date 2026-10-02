# Boot / LCD / ATtiny hardening — 2026-09-29

## Follow-up after hardware feedback (current implementation)

The operator reported complete Tiny connection loss after cf664fa. Its RMT TX
change is rolled back to the proven GPIO + independent esp_timer sender; no
protocol or Tiny firmware replacement is made. The exact RMT failure on that
board is not established without init/failure logs. The earlier host HAL modeled
RMT API success and therefore did not establish peripheral compatibility.

IRAM GPIO reception, fresh link timestamps and decode-before-timeout are kept.
Response timeout now starts from measured GPIO release rather than a nominal
command end. Actual LOW width is checked against the Tiny command windows:
even a valid status frame cannot acknowledge a pulse that drifted into another
command. New diagnostics report measured LOW microseconds when LOG is enabled.
esp_timer uses a software task in this SDK; flash/cache delays of the sender
are not eliminated by IRAM reception. Width validation detects that case; it
does not prove the hardware timing is now fault-free.

Tests now use a wired-AND GPIO model and the actual Tiny command decoder, and
inject timer delays: 235 ms stays a query; 270 ms becomes ACTIVITY_ON and must
not be reported as query success. All 448 control-stall transfers still run.
The HMI splash is changed per explicit confirmation to logo + three dots only.
Startup output interlock and removal of periodic LCD reinitialization remain.

The RMT findings below describe the superseded cf664fa attempt, not the current
sender. Physical verification is still required and no new OTA release is made.

Based on main `29ad4a3`. Wire protocol v4, ATtiny LINKFIX source, pin mapping,
EEPROM layouts, Web transactions, PID and recovery thresholds remain unchanged.

## Findings and fixes

- Startup previously ran `Machine.update()` including held-on light/manual
  controls while HMI still displayed the splash. Coordinator release is now
  separate from HMI acknowledgement of a non-splash frame. Operational outputs,
  resume/turning/tune/test stay inhibited until that acknowledgement. Startup
  frame requires I2C lock, transfer and LCD presence probe. If the LCD is absent
  at boot, normal outputs remain inhibited until it recovers. Once admitted,
  later LCD recovery does not restart the startup interlock.
- Inputs, sensor, RTC, watchdog, fault detection and Tiny continue during boot;
  high-temperature/emergency cooling and emergency siren remain available.
  Final output arbiter applies the interlock independently of normal requests.
- Logo reduced from 69x45 to 49x32, centered at display center. Vietnamese
  status text uses 10-pixel font instead of 12, below the logo.
- Healthy LCD no longer receives the 60-second initialization sequence, idle
  self-heal redraws, or duplicate navigation frames. Dirty/dynamic frames and
  actual I2C recovery remain. ACK checks cannot detect every LCD-internal visual
  corruption; no periodic flashing is used to compensate for that limitation.
- Tiny TX used `ESP_TIMER_TASK` to release LOW: task/cache delays could stretch
  a command into a different command window. RX used Arduino GPIO service;
  the pinned SDK has `CONFIG_ARDUINO_ISR_IRAM` disabled. Flash/NVS writes can
  therefore mask GPIO capture. An `IRAM_ATTR` user callback alone is insufficient.
  RMT now emits the complete command in hardware and releases EOT HIGH, with
  open-drain plus input loopback. Direct GPIO ISR service uses IRAM allocation,
  RAM state and IRAM timestamping. Encoder handlers are converted too: keeping
  Arduino's flash wrapper on this service would introduce cache-disabled panic.
- Bus/controller link timestamps are sampled after storage work, not copied
  from the start of a possibly delayed loop. Complete retained frames are
  decoded before response timeout. Retry, parity, overflow and E501 disconnect
  detection remain enabled; no fault is hidden by a new blanket grace period.

Driver reference: [Espressif RMT TX implementation](https://github.com/espressif/esp-idf/blob/v5.5.4/components/esp_driver_rmt/src/rmt_tx.c).

## Verification and limitations

- Actual C++ bus driver against deterministic GPIO/RMT HAL: all 7 commands x
  16 status flags x 4 foreground stalls (0/300/600/1100 ms), totaling 448 valid
  transfers. Tests cover parity failure, excess edges, missing peer, stuck LOW,
  TX/initialization failure, result-mailbox serialization, timestamp rollover
  and startup output policy. Existing I2C/UART/network/OTA tests still run.
- Linked ESP32 ELF check verifies GPIO handlers/callees in IRAM and edge data,
  encoder table, waveform buffer in internal DRAM. The CI build enforces it.
- Preservation manifest updates are limited to requested boot assets/gate,
  LCD refresh constants, relevant controller/arbiter code, splash/HMI init,
  and the replaced ESP bus implementation. Tiny and unrelated protections keep
  their original hashes. Semantic and fault-injection tests accompany these
  explicitly authorized baseline changes.
- These are code, build and simulated timing checks, not physical measurements.
  Hardware acceptance still requires GPIO41 scope/logic-analyzer capture while
  saving/starting/stopping batches, observing all held switches during splash,
  waiting >60 seconds on a static menu, and disconnecting Tiny to verify E501.
  Keep the existing Tiny LINKFIX firmware; it does not need reflashing for this
  ESP-only timing change. Do not operate live eggs while communication faults
  are active. No new firmware release/tag or Cloudflare deployment is created.
