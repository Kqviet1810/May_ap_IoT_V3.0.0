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
