# MAYAP 3.8.1 — Deployment runbook

## 1. Nguyên tắc

Khách hàng chỉ nhập **Device ID + PIN**. Không yêu cầu nhập broker hostname, port, WSS, MQTT account, Cloudflare token hay Account ID.

Kiến trúc MQTT hiện hành dùng HiveMQ + Worker-authenticated browser session + HMAC cho write channel. Không cấu hình EMQX JWT theo tài liệu v3.8.0 cũ.

## 2. GitHub Actions secrets

### Firmware build/release

Bắt buộc cho build deploy/tag:

- `MAYAP_MQTT_USERNAME`
- `MAYAP_MQTT_PASSWORD`

Bắt buộc khi phát hành tag có ký:

- `MAYAP_OTA_SIGNING_PRIVATE_KEY`

Các giá trị public như CA bundle và OTA public key phải nằm ở cấu hình public/version-controlled phù hợp; private key tuyệt đối không được commit vào firmware.

### Cloudflare deploy workflow

- `CLOUDFLARE_API_TOKEN`
- `CLOUDFLARE_ACCOUNT_ID`

### Worker secrets/runtime

Tối thiểu theo tính năng đang dùng:

- `DEVICE_KEY_PEPPER`
- `VAPID_PUBLIC_KEY`
- `VAPID_PRIVATE_KEY`
- `VAPID_SUBJECT`
- `MAYAP_MQTT_PASSWORD`

Khuyến nghị cấu hình rõ ràng:

- `MAYAP_MQTT_HOST`
- `MAYAP_MQTT_USERNAME`
- `MAYAP_MQTT_WSS_URL` nếu endpoint WSS không theo mặc định `wss://<host>:8884/mqtt`.
- `GITHUB_TOKEN` tùy chọn nếu cần tăng GitHub API rate-limit.

## 3. D1

Áp migrations trước deploy:

```bash
cd cloudflare
npx wrangler d1 migrations apply mayap_push --remote
```

Không dùng `schema.sql` như cách thay thế cho migration history trên production database.

## 4. Worker config

`cloudflare/wrangler.toml` hiện dùng:

- entrypoint `src/reliability-wrapper.js`
- compatibility date `2026-08-01`
- explicit `nodejs_compat`
- D1 `mayap_push`
- cron mỗi phút

Compatibility date này được pin có chủ ý. Chỉ nâng sau regression test; không tự cập nhật theo ngày hiện tại.

Provisioning:

- `REQUIRE_DEVICE_INVENTORY=0`: auto-admit máy mới với rate-limit.
- `REQUIRE_DEVICE_INVENTORY=1`: strict factory allowlist.
- inventory `enabled=0`: blacklist/disable một Device ID cụ thể.

## 5. Build profile

- Push/PR hardening: DEV.
- `workflow_dispatch`: PILOT.
- tag `vX.Y.Z`: PROD.

PROD bắt buộc diagnostic serial OFF và serial input simulation OFF qua build flags CI.

## 6. Release firmware

1. Đảm bảo `release-manifest.json` đúng.
2. Push branch và chờ reliability/build checks xanh.
3. Nạp artifact PILOT vào một máy thật; chạy `doc/COMMISSIONING_V3_8_1.md`.
4. Chỉ sau pilot/soak đạt mới tạo tag, ví dụ `v3.8.1`.
5. Tag phải khớp `MAYAP_FIRMWARE_VERSION`.
6. Workflow compile, ký ECDSA và tạo Release `.bin` + `.sig`.

Không tạo tag mới chỉ để sửa tài liệu; tăng firmware version khi binary/hành vi firmware thay đổi.

## 7. Deploy Worker

Chạy workflow `Deploy Cloudflare Worker` thủ công. Pipeline phải pass:

- JS syntax check;
- release sync checker;
- v3.8.1 reliability checker;
- D1 migrations;
- Wrangler deploy.

Hiện chưa có `cloudflare/package-lock.json`. Workflow sẽ cảnh báo và dùng `npm install --package-lock=false`. Khi lockfile hợp lệ được commit, workflow tự dùng `npm ci`.

## 8. Kiểm tra sau deploy

Tối thiểu:

- máy mới register được theo policy hiện hành;
- Device ID/PIN thêm được trên web mà không nhập broker;
- browser session refresh/check hoạt động;
- browser thứ vượt limit làm revoke client cũ như thiết kế;
- MQTT telemetry đọc được;
- command/config/reminder write được ký và ESP32 chấp nhận;
- sai PIN bị rate-limit;
- Push subscribe/send hoạt động;
- Cloud heartbeat không báo offline giả trong restart ngắn;
- firmware check trả đúng release;
- file sai signature không được nạp.

## 9. Rollback

- Worker: redeploy commit Worker đã xác nhận tốt trước đó; D1 migration cần đánh giá tương thích trước khi rollback code.
- Firmware: dùng OTA rollback/partition flow đã có; không bypass signature.
- Nếu thay broker credential, cập nhật cả firmware release secret và Worker runtime config trước khi rollout.
