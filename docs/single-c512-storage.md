# AT24C512 storage

The single AT24C512 still stores machine configuration, batch state and temperature history at their existing addresses. Generic EEPROM locking, page writes, retries and ACK polling remain unchanged.

Notes and user-created Reminders have no backend or protocol. The previous shared journal, mailbox, startup recovery and realtime routes are removed. Their Web UI remains available with saving disabled until a replacement protocol is implemented.

No erase operation is added. Previously occupied EEPROM ranges remain reserved; existing bytes are not read, migrated or overwritten by these removed features.

Driver regression: `python3 tools/test_single_eeprom.py --sanitize --check-regression`.
