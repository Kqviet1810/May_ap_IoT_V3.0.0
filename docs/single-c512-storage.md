# One AT24C512 at 0x50

This branch uses one external EEPROM, not a primary/backup pair. AT24C512 is
64 KiB (512 **kilobits**), with a 16-bit word address and 128-byte physical pages.
Set A0/A1/A2 low, WP low to enable writes, and retain the common I2C bus/ground.
Do not connect another EEPROM at the same address.

| Region | Address | Purpose |
| --- | --- | --- |
| Config A/B | 0x0000–0x01FF | Existing CRC-checked configuration slots |
| Batch A/B | 0x0200–0x02FF | Existing batch/resume slots |
| Reminders A/B | 0x0300–0x0AFF | Two 1 KiB note/reminder slots |
| Legacy history | 0x0B00–0x0FFF | Left untouched; no automatic history migration |
| New history | 0x1000–0x2F7F | 2016 samples, five minutes each (seven days) |
| Reserved | 0x2F80–0xFFFF | Unallocated; never erased/formatted on boot |

Config, batch and reminder addresses and schemas are intentionally preserved.
Moving these records would require a crash-safe migration, not just changing
constants. Replacing the physical chip does not copy data from the old chip:
an empty C512 starts with defaults. Stop the batch before replacing/flashing.
Do not assume records from the abandoned **dual-chip journal** are compatible;
this branch only reads its own single-chip A/B formats.

Driver capacity arithmetic uses 32 bits (65536 cannot fit in uint16_t).
Each write is bounded by both the physical page and the Wire TX buffer:
maximum 126 data bytes plus two word-address bytes. Reads stay at 32 bytes.
History retention is seven days; the current web request window remains at
most 24 hours to preserve bounded response size/latency.

## Web notes/reminders

The existing `reminders/set` signed MQTT request goes through the HMI transaction
mailbox to `PersistentStore::saveReminders`. RAM is only a runtime cache.
The store alternates A/B slots, validates CRC and compares EEPROM readback before
returning success. Only that success changes the active reminders and produces
the final `applied` ACK. The final report is requeued with the confirmed revision
even if an earlier report raced the save. On restart the newest valid EEPROM
record is loaded; a bad latest CRC falls back to the other slot.

Limits remain ten reminders, each with day and up to 79 UTF-8 **bytes** of text.
Unchanged lists do not write; saving an empty list durably deletes reminders.
No new cloud database or browser-only persistence is introduced.

## Verification

Run `python tools/test_single_eeprom.py` (optional `--sanitize` on Linux).
It compiles the actual production driver and reminder persistence methods
against a C512/Wire fake, testing page crossing, TX capacity, last address,
out-of-range rejection, reboot/clear/no-op, write protection, CRC fallback,
and loss of power after every byte in a reminder update.

Hardware acceptance: add a Vietnamese reminder, wait for confirmed save,
power-cycle both ESP32 and EEPROM, reopen reminders and verify the text. Repeat
for deletion. `[REMIND] save=FAIL` must never be presented as a confirmed save.
This automated simulation is not a substitute for that physical test.
