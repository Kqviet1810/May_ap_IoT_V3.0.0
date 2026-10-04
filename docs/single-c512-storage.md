# One AT24C512 at 0x50

AT24C512 is 64 KiB, with 16-bit addresses and 128-byte physical pages. Keep
A0/A1/A2 low and WP low. No new chip, D1 table or shared fleet secret is required.

| Region | Address | Purpose |
| --- | --- | --- |
| Config A/B | 0x0000–0x01FF | Existing config; unchanged |
| Batch A/B | 0x0200–0x02FF | Existing batch/resume; unchanged |
| Legacy reminders | 0x0300–0x0AFF | Reserved, untouched, no longer read/written |
| Legacy history | 0x0B00–0x0FFF | Untouched |
| Current history | 0x1000–0x2F7F | Existing seven-day history; unchanged |
| Gap | 0x2F80–0x2FFF | Untouched |
| Shared Notes/Reminders journal | 0x3000–0xEFFF | 24 append slots of 2048 bytes |
| Tail | 0xF000–0xFFFF | Untouched |

## Shared journal V2

`note_journal.h` is the storage core. `note_storage.h` runs it from the existing
Arduino loop after local startup settles. Each step performs at most one EEPROM
read or write of at most 32 bytes, without holding a mailbox spinlock over I2C.
It adds no task and does not move config, batch or history records.

The journal retains up to 16 live Notes plus one atomic record containing the
whole list of ten Reminders. Note titles/content retain the existing limits of
60/300 UTF-16 units. Reminder labels retain the existing 79 UTF-8 byte limit.
Slots rotate; current records are protected while obsolete slots are reclaimed.
Deletes append tombstones. A global generation and per-record version fence
stale mutations, including recreation after tombstone reclamation. Unchanged
payloads are read-verified and do not write again.

A reused slot first has its final seal invalidated and read-verified. The body
is written/read-verified in bounded chunks. A separate final-page commit seal
contains the body CRC and its own CRC. The entire committed body is then read
again. Only this final verification permits success. If the driver reports a
failed write but readback proves the bytes were actually written, the journal
continues; mismatches have three bounded attempts per chunk. A damaged committed
record fails closed rather than silently loading an older version. Boot never
formats the chip.

Old Notes and Reminder schemas are not migrated. Their unused bytes are not
bulk-erased, and legacy reminder slots stay reserved. Re-enter existing reminders
when commissioning this firmware. Existing batch/config/history data are retained.

## Shared WebSocket protocol

Presence/bootstrap advertises `notesJournal: 2`. Both features use the existing
per-device, signed Protocol V2 envelope on `notes/request`:

- `notes.list`, `notes.save`, `notes.delete`
- `notes.reminders.read`, `notes.reminders.save`

The old `reminders/set` route and A/B save mailbox/store are removed. Authentication,
ownership, boot/sequence/expiry checks still apply. No new credential is introduced.

The ESP32 sends `notes/reported` with the JSON body as a string and an HMAC-SHA256
signature over this exact newline-separated text (without a final newline):

```
mayap-note-journal:v2
DEVICE_ID
BOOT_ID
REQUEST_ID
OPERATION
BODY
```

The key is the transaction's existing ACK-session key. A terminal Protocol V2
ACK uses `NOTE_JOURNAL_*` codes and the same journal generation in `revision`.
The Web requires both authenticated DATA and terminal ACK with matching generation;
a Hub receipt or unsigned report cannot confirm persistence. DATA/ACK can arrive
in either order. Journal DATA is a transaction reply and remains deliverable to
an authenticated, unexpired browser socket even when its telemetry watch lease
has expired. If a successful signed read ACK arrives without DATA, the Web tries
one fresh READ fenced to that ACK generation; this never re-signs a mutation. An incomplete response times out visibly and leaves the editor
draft; it never claims successful storage. Existing retries retain the exact
signed envelope and request ID. A retry after reconnect/reload rereads storage;
no-op writes are verified without wearing the chip again.

Reminders are restored locally at boot without Internet. A verified Reminder
record is delivered through a bounded mailbox to the controller, which updates
its existing reminder runtime and Cloud Push cache. Only after that application
can the terminal ACK be sent. The runtime scheduling rules remain unchanged.
EEPROM/network completion queues retain results across transient send failures.

## Verification and commissioning

Automated checks:

```
python3 tools/test_note_journal.py
python3 tools/test_single_eeprom.py --sanitize --check-regression
python3 tools/test_runtime_buses.py --sanitize
node --test tests/*.test.cjs
python3 tools/build_web_assets.py
```

The journal tests execute the production core with a byte-addressable EEPROM
model: all 1577 power-cut positions for Notes and again for ten Reminders, false
write-ACK/readback recovery, write protection, capacity, reclamation/reboots,
version conflicts, invalid UTF-8, and preservation of other EEPROM regions.
The driver tests execute the actual Wire driver and journal together, including
the delayed ACK-polling regression. Browser tests exercise the existing UI with
real HMAC DATA/ACK verification and a transport fixture; workerd tests separately
exercise real Worker/DO authentication and routing.

Commissioning requires matching Web/Worker assets and firmware from this branch.
Build assets before deploying the Worker; there is no D1 migration. Flash one
bench ESP32 manually, with heaters safely disabled during storage tests. Re-enter
reminders, save/edit/delete Vietnamese Notes and Reminders, then power-cycle both
ESP32 and EEPROM and verify them. Test offline boot restoration and Wi-Fi loss
while saving. Verify that a failed physical write never produces a success UI.
Check normal config/batch/history preservation and reminder execution afterward.

These host tests prove software failure handling, not the electrical condition,
capacity or writeability of the installed chip. No remote deploy, OTA release or
physical flash is performed by the test commands.

## Journal transaction audit — 2026-10-04

The audit found three source defects behind misleading browser outcomes: transport and `publish()` erased Hub rejection codes; pre-forward Hub rejection paths omitted requestId, leaving the browser's receipt waiter unanswered; presence announced the protocol without announcing mount completion. These are reproducible code defects. They do not establish which stage failed on an unobserved physical device.

Hub errors now retain `code`, `hubCode` and `stage=HUB_REJECTED`. Every pre-forward write rejection includes a bounded requestId for correlation only, without treating the unsigned ID as authentication. BUSY reports its code before the existing close/admission policy. A forwarding receipt still only establishes queuing to the device. Missing receipt or connection loss remains uncertain; known forwarding/device stages use JOURNAL_DEVICE_RECEIPT_MISSING, JOURNAL_COMPLETION_MISSING, JOURNAL_DATA_MISSING or JOURNAL_TERMINAL_ACK_MISSING instead of a generic UNCERTAIN; exact retries continue within the original expiry and attempt bound rather than converting the first missing receipt into immediate failure. Neither the 8-second receipt deadline nor the 30-second journal deadline is extended.

Journal Web logs contain requestId, operation, stage, Hub code, created/published/forwarded/received/completed timestamps and terminal result. Received/completed timestamps in these Web logs use the browser monotonic clock; existing TX latency logs separately identify ESP32 clock values. Serial logs correlate requestId/operation/generation/result/address. The EEPROM driver adds a per-instance read failure snapshot (lock, short read, Wire address failure, missing byte) without changing I/O, locking or retry behavior. No content, title, credentials or signing keys are logged.

For mutations, the browser writes a per-account/device unresolved guard before publication: operation, note ID, original generation/version and SHA-256 of the intended fields. This is not note storage and contains no note content, title, envelope, signature or key. Reload only permits authenticated reads to reconcile this guard; it cannot create another mutation while the old result is unknown. Storage denial or malformed guards fail closed before publication.

After bounded exact retries, an uncertain mutation runs a fresh signed READ. Notes are paginated under a single generation fence, each page requiring authenticated DATA plus authenticated terminal ACK at the same generation. A save succeeds only when ID/title/content/type match its intent hash, its version is not older and generation is not older than the attempted snapshot. Delete requires the ID absent; Reminder reconciliation requires the exact ordered day/label list and nondecreasing generation/version. Reconciliation never re-signs a mutation. Mismatch remains unresolved and blocks writes. Invalid DATA/ACK signatures do not automatically become success: the guard remains for a subsequent verified read.

A terminal I/O failure can occur after writing the seal. It therefore retains the unresolved guard and the verified completed generation. A fresh verified snapshot can confirm the desired state, or establish a different state after the original writer has completed. The latter clears the guard but asks the user to refresh/edit explicitly; it never silently issues another mutation. Expired/replay/auth ACKs alone do not prove a previous attempt was unapplied.

The Web journal ACK handler additionally checks operation, live boot and original request boot before changing any transaction/device state. This prevents an old rejected ACK whose asynchronous HMAC verification finishes after new-boot presence from rolling the current boot ID backwards.

ESP32 `notesJournal=2` remains the capability. New `notesReady` is true only after boot scan/readback finishes successfully; `notesError` identifies mount/storage failures. Status uses the existing mailbox lock and is republished on transition, retrying a failed queue operation. Firmware rejects mutations during mount or failure. Read requests can remount failed storage; Web permits these reads but disables writes. A missing readiness field explicitly requires the new firmware instead of pretending mount is in progress. Offline/expired control rights are checked before firmware support and never misreported as unsupported storage. Mounting is shown as initialization, and readiness changes update the open form without losing its draft.

Web cache is `mayap-web-v1.1.2`; HTML and cached core script URLs share `?v=1.1.2`, separating old unqualified caches. Core assets remain network-first. The EEPROM reservation, on-chip schema, CRC/seal/readback, boot/request/revision fencing, signed DATA/ACK domains, controller/safety behavior, and D1 contents are unchanged. Physical commissioning needs this firmware for readiness fields; there is no remote OTA rollout.

Remaining limits: without a verified terminal result and with a snapshot that differs from intent (for example, a concurrent editor changed it), the browser conservatively keeps the unresolved guard. It cannot infer that an unobserved writer never ran. Losing/clearing browser storage removes the local guard; device generation/version/idempotence and signed request fences remain authoritative. No host test proves the electrical behavior of the installed EEPROM or the original user's transaction outcome.

Validation mapping: normal/Hub refusal/missing received ACK/missing DATA/missing terminal ACK/signature failures/network interruption are covered by the actual app bridge browser fixture and transport tests; reload/uncertain guards and Reminder reconciliation by journal-client tests; exact retry/admission/ownership by DeviceHub and actual runtime/workerd tests; generation/version conflict, capacity, tombstone retries and 4,731 byte-position power cuts by the native journal; mount failure/recovery and bounded physical driver I/O by runtime-note-journal and single-eeprom tests. Browser timeout tests invoke the production timeout handler after a deliberate exact retry; they do not wait 30 seconds or claim WAN/hardware timing.

Changed files in this audit:

- `MAYAP_INDUSTRIAL_v1_0_0/machine_control.h`
- `MAYAP_INDUSTRIAL_v1_0_0/note_mailbox.h`
- `MAYAP_INDUSTRIAL_v1_0_0/note_realtime.h`
- `MAYAP_INDUSTRIAL_v1_0_0/note_storage.h`
- `MAYAP_INDUSTRIAL_v1_0_0/realtime_link.h`
- `app.js`
- `cloudflare/src/device-hub.js`
- `docs/single-c512-storage.md`
- `index.html`
- `journal_client.js`
- `notes.js`
- `realtime_transport.js`
- `release-manifest.json`
- `sw.js`
- `tests/device-hub.test.cjs`
- `tests/journal-client.test.cjs`
- `tests/note-journal.cpp`
- `tests/realtime-transport.test.cjs`
- `tests/runtime-note-journal.cpp`
- `tests/runtime-preservation.json`
- `tests/single-eeprom.cpp`
- `tests/web-experience.test.cjs`
- `tests/web-shell.test.cjs`
- `tools/test_web_experience.cjs`

The journal lifecycle fixtures also retain original request boot metadata, as the production signed publication does; DATA/ACK revision and signature assertions are unchanged. Additional changed test file: `tests/transaction-lifecycle.test.cjs`.
