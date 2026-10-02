-- MAYAP Cloud Push - D1 schema

CREATE TABLE IF NOT EXISTS devices (
  id                  INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id           TEXT NOT NULL UNIQUE,
  device_name         TEXT NOT NULL DEFAULT '',
  device_key_hash     TEXT NOT NULL,
  pairing_token       TEXT,
  created_at          INTEGER NOT NULL,
  last_seen           INTEGER,
  status              TEXT NOT NULL DEFAULT 'unknown',
  batch_running       INTEGER NOT NULL DEFAULT 0,
  web_pin_hash        TEXT
);

-- Allowlist xuat xuong. Thiet bi moi chi duoc /register neu device_id nam o day.
-- browser_limit = NULL -> dung MAX_PAIRED_BROWSERS trong wrangler.toml.
CREATE TABLE IF NOT EXISTS device_inventory (
  device_id       TEXT PRIMARY KEY,
  enabled         INTEGER NOT NULL DEFAULT 1,
  browser_limit   INTEGER,
  created_at      INTEGER NOT NULL DEFAULT 0
);

-- Moi browser co client_id + token hash rieng. Token that chi nam o browser.
CREATE TABLE IF NOT EXISTS device_clients (
  device_id       TEXT NOT NULL,
  client_id       TEXT NOT NULL,
  token_hash      TEXT NOT NULL,
  client_name     TEXT NOT NULL DEFAULT '',
  user_agent      TEXT NOT NULL DEFAULT '',
  issued_at       INTEGER NOT NULL,
  last_seen_at    INTEGER NOT NULL,
  expires_at      INTEGER NOT NULL,
  revoked_at      INTEGER,
  PRIMARY KEY (device_id, client_id)
);
CREATE INDEX IF NOT EXISTS idx_device_clients_active
  ON device_clients(device_id, revoked_at, expires_at, issued_at);

-- 1 push endpoint co the lien ket nhieu device_id. client_id cho phep thu hoi
-- thong bao dung browser khi browser do bi day khoi danh sach duoc cap quyen.
CREATE TABLE IF NOT EXISTS push_subscriptions (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id     TEXT NOT NULL,
  endpoint      TEXT NOT NULL,
  p256dh        TEXT NOT NULL,
  auth          TEXT NOT NULL,
  user_agent    TEXT,
  client_id     TEXT NOT NULL DEFAULT '',
  created_at    INTEGER NOT NULL,
  updated_at    INTEGER NOT NULL,
  UNIQUE(device_id, endpoint)
);
CREATE INDEX IF NOT EXISTS idx_push_subscriptions_device ON push_subscriptions(device_id);
CREATE INDEX IF NOT EXISTS idx_push_subscriptions_device_client
  ON push_subscriptions(device_id, client_id);

CREATE TABLE IF NOT EXISTS alarm_state (
  device_id       TEXT NOT NULL,
  alarm_type      TEXT NOT NULL,
  active          INTEGER NOT NULL DEFAULT 0,
  first_sent_at   INTEGER,
  last_sent_at    INTEGER,
  last_message    TEXT,
  PRIMARY KEY (device_id, alarm_type)
);

CREATE TABLE IF NOT EXISTS alarm_log (
  id                  INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id           TEXT NOT NULL,
  alarm_type          TEXT NOT NULL,
  severity            TEXT NOT NULL,
  state               TEXT NOT NULL,
  message             TEXT NOT NULL,
  temperature         REAL,
  humidity            REAL,
  notification_sent   INTEGER NOT NULL DEFAULT 0,
  created_at          INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_alarm_log_device ON alarm_log(device_id, created_at DESC);

CREATE TABLE IF NOT EXISTS firmware_cache (
  id            INTEGER PRIMARY KEY CHECK (id = 1),
  version       TEXT NOT NULL,
  asset_url     TEXT NOT NULL,
  sha256        TEXT NOT NULL,
  signature     TEXT NOT NULL DEFAULT '',
  size          INTEGER NOT NULL,
  notes         TEXT NOT NULL DEFAULT '',
  fetched_at    INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS auth_rate_limits (
  rate_key TEXT PRIMARY KEY,
  attempts INTEGER NOT NULL DEFAULT 0,
  window_started_at INTEGER NOT NULL,
  blocked_until INTEGER NOT NULL DEFAULT 0,
  updated_at INTEGER NOT NULL
);
