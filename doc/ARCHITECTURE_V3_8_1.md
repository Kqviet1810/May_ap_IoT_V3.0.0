# MAYAP 3.8.1 — Architecture source of truth

Tài liệu này mô tả kiến trúc đang chạy ở nhánh hardening v3.8.1. Nếu tài liệu v3.8.0/JWT/EMQX cũ mâu thuẫn với file này và code hiện hành, dùng **code + tài liệu này** làm chuẩn.

## 1. Các miền thực thi

### ESP32-S3

- `controlTask`: state machine, PID, fault manager, output arbiter.
- `hmiTask`: LCD/encoder và transaction HMI.
- `supervisorTask`: heartbeat/deadline; khi control bất thường thì latch trip, safe outputs và restart.
- `networkTask`: Wi-Fi, MQTT realtime, Cloud HTTPS.
- `otaTask`: ArduinoOTA, Internet OTA, rollback.

Invariant: MQTT/HTTPS không được chạy trong `controlTask`.

### Web PWA

Dashboard kết nối broker bằng WSS cho telemetry/control. `config.js` giữ compatibility layer cho storage/session cũ, nhưng credential MQTT private của phiên không được coi là secret dài hạn để lưu bền trong localStorage.

### Cloudflare Worker

Entrypoint:

```text
reliability-wrapper.js
 -> security-wrapper.js
   -> index.js
```

- reliability wrapper: auto-provision có rate-limit, self-heal reset PIN, có thể bật lại strict inventory.
- security wrapper: browser session, browser limit, rate-limit PIN, revoke session/subscription.
- core worker: device registration, alert/push, MQTT session/config, HMAC signing, firmware metadata/download.

### ATtiny13A

ATtiny dùng pulse protocol v2. Nó giữ batch state fail-safe trong EEPROM và điều khiển còi mất nguồn theo firmware ATtiny hiện hành. Không được dựa vào tài liệu v3.8.0 từng mô tả ATtiny chỉ còn PING/ACK.

## 2. Identity và provisioning

- Device ID: `MAP-` + 12 hex từ ESP32 eFuse MAC.
- Device key: random 256-bit, lưu NVS; nếu persist thất bại thì fail-closed.
- `REQUIRE_DEVICE_INVENTORY=0`: máy mới chưa có inventory record có thể được reliability wrapper auto-admit theo IP rate-limit, sau đó vẫn đi qua security wrapper.
- inventory record `enabled=0`: khóa cụ thể thiết bị.
- `REQUIRE_DEVICE_INVENTORY=1`: chỉ máy đã allowlist mới đăng ký được.

## 3. Browser session

- Browser có `client_id` ổn định.
- Session token 64 hex; token hash lưu D1.
- Session mặc định 90 ngày, cấu hình được 1..365 ngày.
- Browser limit mặc định 3, cấu hình 1..10; inventory có thể override từng máy.
- Khi vượt limit, session active cũ nhất bị revoke cùng push subscription của client đó.
- PIN có cả rate-limit theo device+IP ở core và rate-limit global theo device ở security wrapper.

## 4. MQTT hiện hành

Broker hiện hành là HiveMQ Cloud/Serverless theo cấu hình deploy, **không phải kiến trúc JWT EMQX cũ**.

Web chỉ nhận MQTT config sau xác thực Worker. Các kênh ghi nhạy cảm:

- `command`
- `config/set`
- `reminders/set`

được ký HMAC theo device. Command key được derive server-side từ `DEVICE_KEY_PEPPER` và Device ID; firmware có contract xác minh tương ứng.

Realtime topic root mặc định: `mayap/v1`.

## 5. Cloud alert

ESP32 gửi HTTPS/TLS lên Worker độc lập với MQTT. Worker phát Web Push. ESP32 heartbeat Cloud mặc định 15 s; Worker đang dùng offline threshold 180 s để tránh cảnh báo giả trong chu kỳ OTA/restart có chủ ý.

TLS verification là bắt buộc. Không được dùng `setInsecure()`.

## 6. OTA

### LAN OTA

`ota_update.h`: ArduinoOTA, chỉ enable khi có password.

### Internet OTA

`ota_web_update.h`: máy kiểm release định kỳ hoặc theo lệnh check. Download đi qua Worker/GitHub Release, sau đó kiểm:

1. giới hạn kích thước/thời gian;
2. SHA-256;
3. ECDSA signature bằng public key trong firmware;
4. chỉ `Update.end(true)` sau khi pass.

Web không có quyền tự flash máy. Operator phải xác nhận tại HMI.

## 7. Heater safety

Firmware v3.8.x cắt SSR và heat master/contactors theo fault descriptor/output arbiter. Tuy nhiên đây chỉ là một lớp.

Phần cứng bắt buộc có chuỗi độc lập:

```text
thermal fuse -> thermostat/thermal relay độc lập -> safety contactor -> SSR -> heater
```

Thermostat phải cắt coil contactor trực tiếp, không phụ thuộc ESP32/software.

## 8. Release synchronization

`release-manifest.json` là manifest release. `tools/check_release_sync.py` đối chiếu manifest với firmware/HMI/web/ATtiny/toolchain/workflows và chặn pipeline nếu lệch.

Không dùng số `v3.4.0` trong tên thư mục sketch làm firmware version.
