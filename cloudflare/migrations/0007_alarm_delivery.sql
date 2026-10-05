-- Alarm events only: never used for telemetry. ACK follows the atomic batch.
CREATE TABLE IF NOT EXISTS alarm_events (
  order_seq INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT NOT NULL,
  event_id TEXT NOT NULL,
  alarm_type TEXT NOT NULL,
  payload TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  UNIQUE(device_id, event_id)
);
CREATE TABLE IF NOT EXISTS alarm_deliveries (
  event_seq INTEGER NOT NULL REFERENCES alarm_events(order_seq) ON DELETE CASCADE,
  endpoint TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'pending',
  attempts INTEGER NOT NULL DEFAULT 0,
  next_attempt_at INTEGER NOT NULL,
  lease_token TEXT,
  lease_until INTEGER NOT NULL DEFAULT 0,
  last_status INTEGER,
  PRIMARY KEY(event_seq, endpoint)
);
CREATE INDEX IF NOT EXISTS alarm_deliveries_due ON alarm_deliveries(status, next_attempt_at);
CREATE INDEX IF NOT EXISTS alarm_events_expiry ON alarm_events(expires_at);
