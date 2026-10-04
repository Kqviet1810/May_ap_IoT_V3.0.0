-- Cloud source-of-truth for user Notes and custom Reminders.
-- ESP32/AT24C512 is intentionally not part of this persistence path.
CREATE TABLE cloud_notes (
  id TEXT PRIMARY KEY,
  device_id TEXT NOT NULL REFERENCES devices(device_id),
  note_type TEXT NOT NULL CHECK(note_type IN ('machine','batch')),
  title TEXT NOT NULL DEFAULT '' CHECK(length(title) <= 60),
  content TEXT NOT NULL CHECK(length(content) BETWEEN 1 AND 300),
  version INTEGER NOT NULL DEFAULT 1 CHECK(version >= 1),
  last_mutation_id TEXT NOT NULL,
  created_by TEXT NOT NULL REFERENCES users(google_sub),
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  deleted_at INTEGER
);
CREATE INDEX idx_cloud_notes_device_active
  ON cloud_notes(device_id, deleted_at, updated_at DESC);
CREATE UNIQUE INDEX idx_cloud_notes_device_mutation
  ON cloud_notes(device_id, last_mutation_id);

CREATE TABLE cloud_reminders (
  id TEXT PRIMARY KEY,
  device_id TEXT NOT NULL REFERENCES devices(device_id),
  incubation_day INTEGER NOT NULL CHECK(incubation_day BETWEEN 1 AND 99),
  label TEXT NOT NULL CHECK(length(label) BETWEEN 1 AND 120),
  version INTEGER NOT NULL DEFAULT 1 CHECK(version >= 1),
  last_mutation_id TEXT NOT NULL,
  created_by TEXT NOT NULL REFERENCES users(google_sub),
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  deleted_at INTEGER
);
CREATE INDEX idx_cloud_reminders_device_active
  ON cloud_reminders(device_id, deleted_at, incubation_day, updated_at);
CREATE UNIQUE INDEX idx_cloud_reminders_device_mutation
  ON cloud_reminders(device_id, last_mutation_id);
