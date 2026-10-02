-- Browser/session hardening for MAYAP online security.
-- Existing registered devices are automatically trusted into inventory so the
-- deployment does not break machines already in service.

CREATE TABLE IF NOT EXISTS device_inventory (
  device_id TEXT PRIMARY KEY,
  enabled INTEGER NOT NULL DEFAULT 1,
  browser_limit INTEGER,
  created_at INTEGER NOT NULL DEFAULT 0
);

INSERT OR IGNORE INTO device_inventory (device_id, enabled, browser_limit, created_at)
SELECT device_id, 1, NULL, COALESCE(created_at, 0) FROM devices;

CREATE TABLE IF NOT EXISTS device_clients (
  device_id TEXT NOT NULL,
  client_id TEXT NOT NULL,
  token_hash TEXT NOT NULL,
  client_name TEXT NOT NULL DEFAULT '',
  user_agent TEXT NOT NULL DEFAULT '',
  issued_at INTEGER NOT NULL,
  last_seen_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  revoked_at INTEGER,
  PRIMARY KEY (device_id, client_id)
);

CREATE INDEX IF NOT EXISTS idx_device_clients_active
  ON device_clients (device_id, revoked_at, expires_at, issued_at);

ALTER TABLE push_subscriptions ADD COLUMN client_id TEXT NOT NULL DEFAULT '';
CREATE INDEX IF NOT EXISTS idx_push_subscriptions_device_client
  ON push_subscriptions (device_id, client_id);
