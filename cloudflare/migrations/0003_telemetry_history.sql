-- Telemetry nhiet do/do am dung cho bieu do lich su ngan han tren web.
-- recorded_at lay tu Worker (Date.now), khong phu thuoc RTC/NTP cua ESP32.
CREATE TABLE IF NOT EXISTS telemetry_history (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id     TEXT NOT NULL,
  recorded_at   INTEGER NOT NULL,
  temperature   REAL NOT NULL,
  humidity      REAL,
  batch_running INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_telemetry_device_time
  ON telemetry_history(device_id, recorded_at);
CREATE INDEX IF NOT EXISTS idx_telemetry_time
  ON telemetry_history(recorded_at);
