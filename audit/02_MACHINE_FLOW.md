# 02 — RECONSTRUCTED MACHINE BEHAVIOUR

Reconstructed from source only (no documentation used). Evidence: **STATIC**.
Commit `f3ec7cb`, branch `main`.

## 2.1 Actual boot / lifecycle flow

```text
POWER ON
 └─ setup(): mayapSafeOutputsEarly() (all 9 relays OFF before pinMode)
     └─ Machine.begin()
         ├─ PowerManager: esp_reset_reason() + reset-storm counter (NVS)
         │    -> AbnormalReset latched when counter >= RESET_STORM_LIMIT(3)
         ├─ store_.begin() -> loadConfig() (A/B + CRC, legacy V3..V7 migration)
         │    -> StorageUnavailable latched if EXTERNAL_EEPROM_REQUIRED and fails
         ├─ loadBatch() + safetyJournal_.stopIntentPending()
         │    ├─ stopIntent && record.wasRunning==0  -> clear tombstone, clean boot
         │    ├─ stopIntent && (record.wasRunning==1 || unreadable)
         │    │      -> batchClearPending_ = true      (NO resume, ever)
         │    └─ !stopIntent && record.wasRunning==1
         │           -> resumePending_ = true
         │              resumeConfirmationRequired_ =
         │                 resetReasonIsPowerInterruption && !autoResumeOnPowerLoss
         └─ heatRestartNotBefore_ = boot + powerRestoreDelaySec
IDLE / READY  (batchPhase_ = Stopped)
 ├─ READY AUTO      (AUTO switch on)   — startBatch permitted if all gates pass
 └─ READY MANUAL    (AUTO switch off)  — manual fan/light/turn switches live
START (HmiCommandType::BatchStart, from HMI button or MQTT `batch_start`)
 └─ startBatch(): 16 gate checks -> saveBatchRecord() must succeed, else full rollback
PRESTART  (batchPhase_ = Prestart, FAN_PRESTART_MS = 5 s)
 ├─ circulation fan ON (staggered from the master contactor by
 │   CIRC_FAN_BATCH_START_STAGGER_MS), SSR still blocked by fanAllowsHeat
 └─ automatic turning fully suppressed in Prestart
HOMING    (only if needHome_ && AUTO)   -> drives toward HOME_TO_LEFT limit
 └─ finishHoming(): needHome_=false, phase->Running, setTurnScheduleAnchor()
RUNNING   (batchPhase_ = Running)
 ├─ HEATING  : PID -> ssrWindowOn -> heaterSsr, contactor per heater switch
 ├─ TURNING  : nextTurnAt_ -> requestTurn -> Deadtime(500 ms) -> Moving -> limit
 │             -> completeTurn -> counters++ -> setTurnScheduleAnchor ->
 │                saveBatchRecord()
 ├─ CHECKPOINT: checkpointBatch() every BATCH_CHECKPOINT_MS (5 min)
 └─ LOCKDOWN : last TURN_LOCKDOWN_DAYS (3) days -> automatic turning suppressed
BATCH END
 └─ *** NO AUTOMATIC END STATE EXISTS *** — see finding F-06.
    At day >= totalIncubationDays only FaultCode::BatchOverdue (Warning) is raised;
    heating, fans and lockdown continue until a human sends Stop.
STOP (BatchStop command, or ResumeNo while waiting for confirmation)
 ├─ safetyJournal_.setStopIntent()  (tombstone written FIRST)
 ├─ batchClearPending_ = true, RAM state zeroed, stopTurn(), postCool 10 s
 └─ tryCommitBatchClear() -> clearBatchRecord() -> on success clear tombstone
READY FOR NEXT BATCH (batchPhase_ = Stopped; start blocked while
                      batchClearPending_ is still true)
```

## 2.2 Batch state table

| State | Entry | Action | Exit | Timeout | Fault | Recovery |
|---|---|---|---|---|---|---|
| Stopped | boot, stopBatch(), failed startBatch() | no heat (batchAllowsHeat=false), manual fan/light/turn honoured when AUTO off | startBatch() / processResume() | none | — | n/a |
| Prestart | startBatch(), processResume() | fan ON, SSR blocked, auto-turn suppressed | FAN_PRESTART_MS elapsed in updateBatchTime() | 5 s fixed | inherits all | — |
| Homing | Prestart exit with needHome_ && AUTO | drive to HOME_TO_LEFT limit | limit reached -> finishHoming() | turnMaxRunSec + TURN_LIMIT_RELEASE_TIMEOUT_MS | TurnTimeout / TurnLimitStuck | ACK -> clearTurnFault() |
| Running | Prestart/Homing exit | PID heat, scheduled turning, checkpoints, batch log | stopBatch() only | none (no batch-end timeout) | any | per fault |
| resumePending_ (pseudo-state) | boot with wasRunning=1 | all actuators cut while resumeConfirmationRequired_; otherwise gated wait | ResumeYes/ResumeNo, or all gates pass | none (E135 warns after 15 min) | ResumeConfirmationPending, ResumeRtcWaitTooLong | operator action |
| batchClearPending_ (pseudo-state) | stopBatch(), ResumeNo, hostile boot | immediateMasterDrop, start/resume blocked | EEPROM clear verified | retry every 3 s forever | BatchStateClearPending (Stop) | EEPROM returns |
| testModeActive_ | TestModeEnter (only when no batch) | manual per-output pulses, limit-switch verification | TestModeExit, or TEST_MODE_IDLE_EXIT_MS idle | idle timeout | — | forceSafe on exit |

## 2.3 Turning state table

| State | Entry | Action | Exit | Timeout | Fault | Recovery |
|---|---|---|---|---|---|---|
| Idle | boot, stopTurn(), completeTurn() | none | requestTurn() | — | — | — |
| DeadtimeLeft/Right | requestTurn() | both motor outputs OFF | TURN_DIRECTION_DEADTIME_MS (500 ms) | — | — | — |
| MovingLeft/Right | beginPhysicalMove() | one direction relay ON (arbiter re-checks interlock) | target limit -> completeTurn() | turnMaxRunSec (5..600 s); origin limit must release within 2.5 s | TurnTimeout, TurnLimitStuck | ACK -> clearTurnFault() |
| Fault | latchTurnFault() | motor outputs OFF (req.turnLeft/Right derived from phase) | clearTurnFault() via AlarmAck | — | TurnLimitConflict / TurnTimeout / TurnLimitStuck / TurnCommandConflict | ACK, plus Test-Mode check after 3 consecutive timeouts/stucks |

## 2.4 Structural checks demanded by §11

* **Unreachable state** — none found. Every BatchPhase and TurnPhase value has at
  least one writer and one reader.
* **State with no exit** — `TurnPhase::Fault` exits only through
  `clearTurnFault()` (AlarmAck). `TurnMechanicalCheckRequired` (E205) exits only
  through Test Mode, which is itself unreachable while a batch runs → see F-02/F-03.
* **Conflicting transitions** — `batchRunning_ && resumePending_` cannot coexist:
  `processResume()` clears `resumePending_` in the same statement block that sets
  `batchRunning_`; `begin()` sets at most one of them.
* **Missing transition** — no `Running -> Complete` transition exists (F-06).
* **Stale state after re-entry** — `startBatch()` re-initialises elapsed, counters,
  schedule, tray position, low-temp/humidity timers. It does **not** re-initialise
  `turnMechanicalCheckRequired_`, `turnFaultStreak_`, `heaterStuckTracking_`,
  `tempOscillationCrossCount_`, `tempRateRefValue_`, `lastTurnCounterDay_`,
  `lightWebOverrideActive_`, `sirenMutedUntil_`. Only the first of those has an
  operational consequence (F-02).
* **Output held from previous state** — no. Every cycle recomputes a full
  `OutputRequest` from scratch; `OutputArbiter` is the single writer and holds
  outputs only through explicit min-on / min-switch / pickup timers, all of which
  are bypassed by `forceAllSafe`/system trip.
* **Transition depending on an unreliable flag** — `needHome_` and `trayPosition_`
  are RAM-only and are recomputed from the physical limit switches at every entry
  point (`startBatch`, `processResume`, `processInputModeTransition`,
  `clearTurnFault`). The one path that does *not* recompute them is
  `latchTurnFault()` (F-10).
