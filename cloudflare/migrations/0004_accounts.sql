-- Legacy browser secrets cannot establish ownership. Owners claim once after login.
CREATE TABLE users (
  google_sub TEXT PRIMARY KEY, email TEXT NOT NULL DEFAULT '', name TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL, last_login_at INTEGER NOT NULL, disabled INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE user_sessions (
  id TEXT PRIMARY KEY, user_sub TEXT NOT NULL REFERENCES users(google_sub),
  token_hash TEXT NOT NULL UNIQUE,
  created_at INTEGER NOT NULL, expires_at INTEGER NOT NULL, revoked_at INTEGER,
  user_agent TEXT NOT NULL DEFAULT ''
);
CREATE INDEX idx_user_sessions_user ON user_sessions(user_sub, expires_at);
CREATE TABLE user_devices (
  user_sub TEXT NOT NULL REFERENCES users(google_sub), device_id TEXT NOT NULL REFERENCES devices(device_id),
  role TEXT NOT NULL CHECK(role IN ('owner','operator','viewer')),
  created_at INTEGER NOT NULL, PRIMARY KEY(user_sub, device_id)
);
CREATE UNIQUE INDEX idx_device_one_owner ON user_devices(device_id) WHERE role = 'owner';
CREATE INDEX idx_user_devices_device ON user_devices(device_id);
CREATE TABLE google_login_challenges (
  challenge_hash TEXT PRIMARY KEY, nonce TEXT NOT NULL, expires_at INTEGER NOT NULL
);
ALTER TABLE push_subscriptions ADD COLUMN user_sub TEXT REFERENCES users(google_sub);
ALTER TABLE push_subscriptions ADD COLUMN user_session_id TEXT REFERENCES user_sessions(id);
-- Old subscriptions were authorized by a browser pairing secret, not an account.
DELETE FROM push_subscriptions;
UPDATE device_clients SET revoked_at = COALESCE(revoked_at, unixepoch() * 1000);
-- device_inventory remains the factory admission/disable list. browser_limit and
-- device_clients are legacy audit records; no account route authorizes through them.
