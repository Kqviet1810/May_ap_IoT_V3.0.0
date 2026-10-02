# 04 — PERSISTENCE + POWER-LOSS MATRIX

Evidence: **STATIC** (full read of `ExternalEeprom24xx`, `PersistentStore`,
`SafetyJournal`, `PowerManager`, `MachineController::begin/processResume/
saveBatchRecord/clearBatchRecord/tryCommitBatchClear`) plus **SIMULATED**
(`audit/sim/timing_model.cpp` for the elapsed-time arithmetic).

## 4.1 Persistent-storage implementation (§24 — read, not assumed)

```text
Record layout (identical shape for Config / Batch / Reminders):
  { magic, schema, size, sequence(uint32), payload, crc32(over everything
    before crc) }

Validation (validConfig/validBatch/validReminders):
  magic OK && schema OK && size == sizeof(record) && crc32 recomputed matches.

Write path (saveBatch/saveConfig/saveReminders):
  1. sanitize (config/reminders only) and pack
  2. if the packed payload is byte-identical to the cached one -> NO WRITE AT ALL
  3. record.sequence = cachedSequence + 1 (or 1 if no valid cache)
  4. target slot = the slot that is NOT current  (true ping-pong, never in place)
  5. writeRecord() -> ExternalEeprom24xx::writeBytes (page-aware, 3 attempts,
     ACK-polling for write completion)
  6. readRecord() back, re-validate, compare sequence AND payload byte-for-byte
  7. only then update the RAM cache / current-slot pointer
  A failure at any step returns false and leaves BOTH slots in their prior state
  except the half-written target slot, whose CRC will simply not validate.

Read path (refresh*Cache):
  read A and B, validate each, pick: if only one valid -> that one; if both valid
  -> the one with the newer sequence, using wrapping comparison
  `static_cast<int32_t>(a-b) > 0`. Config additionally falls back through legacy
  schemas V7,V6,V5,V4,V3 in that order when no V8 record validates.
```

**Answer to "after reboot, which record is chosen and why?"** — the slot whose
record passes magic+schema+size+CRC and carries the newer wrapping sequence. A
torn write only ever damages the *non-current* slot, so the previous good record
always survives. Verified by reading the implementation, not by analogy.

Checks from §24:

| Scenario | Behaviour | Verdict |
|---|---|---|
| power loss before write | nothing changed | SAFE |
| power loss during write | target = other slot, CRC fails on reload, current slot still valid | SAFE |
| power loss after write, before verify | record is complete and valid; sequence is newer; adopted on next boot | SAFE |
| partial write (page boundary) | writeBytesOnce is page-aware; partial data fails CRC | SAFE |
| corrupt record | CRC/magic/size reject | SAFE |
| old record | lower sequence loses | SAFE |
| invalid CRC | rejected | SAFE |
| version mismatch | Config migrates V3..V7 → V8; **Batch has only V1 + legacy V2, no forward migration beyond that** | ACCEPTABLE |
| storage unavailable | `EXTERNAL_EEPROM_REQUIRED=true` → StorageUnavailable latched, start/resume blocked, immediateMasterDrop | SAFE |
| write failure | streak ≥ 3 → StorageUnavailable, but **only when no batch is running**; during a batch it stays "degraded" and control continues from RAM by design | ACCEPTABLE (documented) |
| read failure | same as above | ACCEPTABLE |
| sequence wrap | wrapping signed comparison is correct | SAFE |

**Note:** `SafetyJournal` uses ESP32 NVS (`Preferences`), which has its own
wear-levelled, power-safe implementation; the firmware additionally read-backs
every write (`setStopIntent`/`clearStopIntent`/`setResetCount` all verify).

## 4.2 Power-loss injection matrix (§25)

Notation for "Resume decision": **RESUME** = `resumePending_` set; **CONFIRM** =
resume held until operator answers CO/HUY; **CLEAR** = `batchClearPending_`, no
resume possible; **CLEAN** = nothing to restore.

| Loss injected at | Persisted state at that instant | Decision on reboot | Outputs during the gap | Verdict |
|---|---|---|---|---|
| IDLE (no batch) | wasRunning=0, no tombstone | CLEAN | all safe (mayapSafeOutputsEarly first line of setup) | PASS |
| START — between `batchRunning_=true` and `saveBatchRecord()` | still wasRunning=0 | CLEAN (batch lost) | safe | PASS (fail-safe direction; the batch simply never started) |
| START — after saveBatchRecord | wasRunning=1 | RESUME / CONFIRM | safe | PASS |
| PRESTART | wasRunning=1, elapsed≈0 | RESUME → Prestart again | safe | PASS |
| HOMING | wasRunning=1 | RESUME → tray position re-derived from limits, `needHome_` recomputed | safe | PASS |
| TURNING dead-time | wasRunning=1 | RESUME; `nextTurnAt_=0` → rescheduled from `lastTurnEpoch_` | both motor relays OFF | PASS |
| TURNING mid-move | wasRunning=1; tray physically mid-travel | RESUME; `trayPosition_` = Left/Right if a limit is closed, else Unknown → `needHome_=true` → homing move before any counted turn | both motor relays OFF, arbiter enforces dead-time on restart | PASS |
| HEATING | wasRunning=1; elapsed from last checkpoint (≤5 min stale) | RESUME; `adjustResumeElapsedFromRtc()` adds the real outage duration from `checkpointEpoch` (rejected if epoch went backwards or gap > 45 days) | heat off until `powerRestoreDelaySec` + FAN_PRESTART_MS | PASS |
| FAULT ACTIVE | faults are RAM-only | RESUME; faults re-evaluated from live inputs | safe | PASS — but note **E205 mechanical lockout is also RAM-only and is erased by the reboot** (F-07) |
| ACK in progress | NVS reset counter is read-back verified | as per counter | safe | PASS |
| CONFIG SAVE | ping-pong slot | old config retained, or new config fully valid | safe | PASS |
| BATCH CHECKPOINT | ping-pong slot | previous checkpoint retained (≤5 min older) + RTC compensation | safe | PASS |
| STOP — after `setStopIntent()`, before EEPROM clear | tombstone present, wasRunning=1 | **CLEAR** — `batchClearPending_`, no resume, retries the erase every 3 s | immediateMasterDrop | PASS (stop intent is never lost) |
| STOP — after EEPROM clear, before tombstone erase | tombstone present, wasRunning=0 | tombstone erased at boot, CLEAN | safe | PASS |
| BATCH CLEAR retry loop | tombstone + wasRunning=1 | CLEAR again | immediateMasterDrop | PASS |
| BATCH COMPLETE | *no such state exists* | n/a | n/a | see F-06 |

### Specific hazards named by §25

* **Accidental auto-resume** — not found. A batch resumes only if
  `wasRunning==1` **and** no stop tombstone. After a true power interruption
  (`ESP_RST_POWERON`/`BROWNOUT`/`UNKNOWN`) the operator must confirm, unless they
  deliberately enabled `autoResumeOnPowerLoss`.
* **Lost stop intent** — not found. The tombstone is written *before* any RAM or
  EEPROM mutation in `stopBatch()` and `ResumeNo`, and is erased only after the
  `wasRunning=0` record has been read back and verified.
* **Stale batch resurrection** — not found; blocked by the same tombstone.
* **Wrong elapsed time** — bounded. Worst case without a valid RTC is one
  checkpoint interval (5 min) lost per outage; with a valid RTC the outage is
  added back exactly. Overflow is clamped at UINT32_MAX (simulated, PASS).
* **Wrong tray position** — not found. Position is never trusted from EEPROM; it
  is re-derived from the limit switches at every resume/start/AUTO transition.
* **Wrong turn schedule** — `lastTurnEpoch_` is persisted and the schedule is
  rebuilt from it. If the RTC is invalid at that moment the code waits up to one
  full interval for the RTC rather than granting a fresh interval.
* **Heating before validation** — not found. `heatRestartNotBefore_` = boot +
  `powerRestoreDelaySec`, `sensorUsable_` requires
  `SENSOR_RECOVERY_GOOD_SAMPLES`, `fanAllowsHeat` requires FAN_PRESTART_MS of
  stable fan, and `processResume()` will not even leave the waiting state until
  the sensor, RTC, AUTO switch, heater switch and temperature gates all pass.
