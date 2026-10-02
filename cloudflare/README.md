# MAYAP Cloudflare Worker — v3.8.1

Backend hiện hành cho provisioning, browser session, MQTT web bootstrap/signing, trạng thái thiết bị, firmware metadata và Web Push.

## Entrypoint

```text
src/reliability-wrapper.js
  -> src/security-wrapper.js
     -> src/index.js
```

Không deploy trực tiếp `index.js` làm entrypoint nếu muốn giữ hardening/reliability hiện hành.

## Cài phụ thuộc

`package.json` đang pin dependency trực tiếp. Repo hiện chưa có `package-lock.json`.

```bash
cd cloudflare
npm install --ignore-scripts
```

Khi đã tạo và review `package-lock.json`, commit lockfile; workflow deploy sẽ tự chuyển sang:

```bash
npm ci --ignore-scripts
```

Không tạo lockfile động trong production deploy rồi coi đó là reproducible build.

## D1

Database binding: `DB`, database name `mayap_push`.

Production deploy dùng migration history:

```bash
npx wrangler d1 migrations apply mayap_push --remote
```

## Secrets

Đặt bằng Cloudflare Dashboard hoặc `wrangler secret put`; không commit secret vào `wrangler.toml`.

Tối thiểu theo tính năng:

```text
DEVICE_KEY_PEPPER
VAPID_PUBLIC_KEY
VAPID_PRIVATE_KEY
VAPID_SUBJECT
MAYAP_MQTT_PASSWORD
```

Khuyến nghị cấu hình tường minh thêm:

```text
MAYAP_MQTT_HOST
MAYAP_MQTT_USERNAME
MAYAP_MQTT_WSS_URL   # tùy chọn nếu URL không theo host:8884/mqtt
GITHUB_TOKEN         # tùy chọn
```

## Provisioning policy

`wrangler.toml` hiện mặc định:

```text
REQUIRE_DEVICE_INVENTORY=0
MAX_NEW_DEVICE_REGISTRATIONS_PER_HOUR=20
NEW_DEVICE_REGISTRATION_WINDOW_MINUTES=60
MAX_PAIRED_BROWSERS=3
BROWSER_SESSION_DAYS=90
```

Với inventory=0, máy mới có device credentials hợp lệ được reliability wrapper auto-admit theo IP rate-limit và tạo inventory record. Nếu một Device ID đã có inventory record `enabled=0`, máy đó vẫn bị chặn. Đặt `REQUIRE_DEVICE_INVENTORY=1` để bật strict factory allowlist.

## MQTT

Kiến trúc hiện hành **không dùng EMQX JWT**.

Worker trả MQTT config cho browser đã xác thực. Write channel quan trọng được HMAC theo Device ID:

```text
command
config/set
reminders/set
```

Không chuyển broker password vào source web tĩnh.

## Web Push

Worker nhận alert/heartbeat từ ESP32 qua HTTPS. Push dùng VAPID. Alarm state/cooldown được giữ phía server để firmware lỗi không tạo push storm vô hạn.

Cron chạy mỗi phút để đánh giá trạng thái online/offline; threshold hiện hành trong core Worker là 180 giây.

## OTA

Nguồn release là GitHub Releases của repo. Worker lấy metadata/file cho ESP32; firmware tự kiểm SHA-256 + ECDSA signature. Private signing key không thuộc Worker/firmware source.

## Compatibility date

`wrangler.toml` đang pin `2026-08-01` + `nodejs_compat`. Giữ nguyên cho đến khi có regression test khi nâng runtime; không tự đổi theo ngày deploy.

## Deploy

Khuyến nghị dùng workflow `.github/workflows/deploy-cloudflare-worker.yml` thay vì deploy tay. Workflow chạy syntax check, release-sync checker, reliability checker, D1 migrations rồi mới `wrangler deploy`.
