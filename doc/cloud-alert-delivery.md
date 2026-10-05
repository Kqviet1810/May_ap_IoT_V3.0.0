# Cloud alarm delivery

Firmware records actual fault condition transitions in a 64-edge fixed mailbox,
independent of HMI's 12-entry display and Cloud's blocking HTTPS operation. Control
only enters a short RAM critical section; it never waits for TLS/D1/Push. There are
48 current-state slots. Overflow is counted and resynchronizes the affected current
states; finite RAM cannot preserve unlimited transitions during a prolonged outage.
The 16-entry Cloud queue reserves four slots for critical events and applies backpressure rather than evicting queued faults.
Critical events pass routine messages in both the mailbox and send queue while preserving each alarm's transitions. Notification producer flags advance only after queue admission.
Submitted event payloads remain immutable across retries. Retry backoff is per event,
so a failed routine event cannot hold a new critical alarm.

All alarms can request a bounded TLS RAM handoff, bypassing heartbeat's 60-second
cooldown. Only the realtime owner closes its socket. A lease lasts 15 seconds and
stays active while Cloud owns TLS; a failed urgent lease has a five-second recovery
gap before another request (20 seconds from its start). Successful completion
releases the urgent retry restriction. Memory admission remains 73728/24576 bytes.
Actual admission deferral does not consume the three-second normal send gap.

Each event has a random 64-bit boot identifier plus a 32-bit sequence. Firmware
removes it only after a matching `success:true,durable:true,event_id` response.
HTTP 200 alone, Push provider success, and OS display are different outcomes.

Migration `0007_alarm_delivery.sql` adds alarm-only D1 events and recipient jobs.
The existing deployment workflow applies migrations before deploying Worker.
Queue ingestion is atomic; retrying an event returns its original durable receipt.
Delivery claims have expiring leases, four-way bounded concurrency, a four-second
provider timeout, retry backoff and a one-hour delivery TTL. Immediate bounded
attempts (with 2s/10s retry delays) run under `waitUntil`; the existing minute cron resumes persisted jobs.
A later transition waits behind an earlier pending transition on the same phone.
Ownership is rechecked before every delivery. Gone endpoints are removed; transient
failures remain pending. Receipts are retained seven days. No new secrets/resources
besides the two tables in the existing D1 database are needed.

Deploy Worker + migration before flashing the new firmware. Old firmware continues
to work against the new Worker. New firmware against an old Worker retains alerts
because the old response cannot satisfy the durable receipt contract.

Diagnostics: firmware logs event ID, age and durable ACK; Worker logs persistence
and per-recipient provider acceptance/retry; service worker logs successful
`showNotification` calls. No credential, endpoint or complete header is logged by
the new event diagnostics. Provider acceptance does not prove an OS displayed it.
OS power saving, Focus/offline state and Apple/Google infrastructure remain outside
our latency guarantees. An interrupted request after provider acceptance can still
cause a retry; stable notification tags collapse duplicates, but Web Push has no
transactional exactly-once display guarantee. ACTIVE and RESOLVED tags are separate.

Commissioning: trigger FAULT_130 with an active batch, record event IDs and stage
latencies; repeat while WS is connected and immediately after heartbeat. Test two
phones, rapid active/resolved transitions, weak/lost Wi-Fi and reconnect. Verify
local HMI/heater trips immediately, no controller reboot, and stored delivery jobs
resume after recovery. Do not use the browser Test button as an end-to-end ESP test.
