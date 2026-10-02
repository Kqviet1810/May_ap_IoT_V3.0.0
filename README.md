# MAYAP — Máy ấp trứng thông minh

> Baseline đã audit: **MAYAP release 1.0.0** trên ESP32-S3-WROOM-1U-N8, Web PWA và Cloudflare Worker. Xem `doc/RELEASE_1_0_0.md` về giới hạn kiểm thử.

Firmware bổ sung staged startup, chẩn đoán RTC và adaptive recovery Level 0–3. Boot flow, thời gian, file thay đổi và giới hạn phần cứng: [Adaptive Staged Boot](doc/ADAPTIVE_STAGED_BOOT.md).

Runtime recovery cho shared I2C, RS485, service task và Wi-Fi deep recovery: [Runtime Self-Recovery](doc/RUNTIME_SELF_RECOVERY.md). Tài liệu nêu ladder, owner/mutex, điều kiện restart và giới hạn kiểm thử.

Nhánh migration dự kiến **V1.1.0** chuyển realtime sang Cloudflare WebSocket + SQLite Durable Objects và đưa Web PWA lên Workers Static Assets. ESP32 vẫn là controller duy nhất. Account/API/D1/Push và signed command/ACK V2 được giữ; không còn broker hoặc credential fleet. Chưa phát hành OTA. Xem [kiến trúc, cấu hình, quota, test và commissioning](doc/CLOUDFLARE_REALTIME_MIGRATION.md).

| Thành phần | Phiên bản hiện hành |
|---|---:|
| Release dự kiến | 1.1.0 |
| ESP32 firmware | 1.1.0 |
| HMI firmware | 1.0.0 |
| Web cache | 1.1.0 |
| ATtiny protocol | 4 |
| ESP32 Arduino core CI | 3.3.11 |
| Arduino CLI CI | 1.5.1 |
| Node CI | 24 |

Các giá trị trên được khai báo ở `release-manifest.json` và được CI đối chiếu với source bằng `tools/check_release_sync.py`. Không sửa một phiên bản đơn lẻ mà không cập nhật manifest/checker tương ứng.

## Kiến trúc hiện hành

```text
Web PWA (Workers Static Assets) -- WSS --> DeviceHub / máy <-- WSS -- ESP32-S3
               |                          (routing + lease)           |
               +-- HTTPS --> Worker account/API/D1/Push <-- HTTPS ------+
                                   |
                       GitHub source / CI / deployment

ESP32-S3 <---- pulse-width protocol v4 ----> ATtiny13A
```

- ESP32 điều khiển PID/heater/safety/turning/alarm/batch; Internet mất vẫn chạy độc lập.
- Hub kiểm identity/ownership và chuyển gói tin; chỉ ACK có HMAC từ ESP32 xác nhận kết quả.
- Telemetry dùng socket/attachment có giới hạn; D1 chỉ làm control plane.
- Cloud tiếp tục provisioning, account, PIN, Push, trạng thái và metadata OTA.
- OTA Internet vẫn cần xác nhận HMI, SHA-256 và ECDSA; PR này không phát hành OTA.
- Yêu cầu phần cứng an toàn giữ nguyên: [SAFETY_HARDWARE_REQUIREMENTS](doc/SAFETY_HARDWARE_REQUIREMENTS.md).

## Trải nghiệm người dùng

Người dùng cuối **không nhập hostname, port, WebSocket URL hay Cloudflare token**. Luồng chuẩn là:

1. Máy tạo Device ID dạng `MAP-XXXXXXXXXXXX` từ eFuse MAC.
2. ESP32 có device key 256-bit riêng, lưu NVS.
3. Máy đăng ký/provision qua Worker và đồng bộ PIN Web.
4. Trên Web cùng origin Cloudflare, người dùng đăng nhập **Google**, claim máy lần đầu bằng **Device ID + PIN**.
5. Worker cấp MAYAP account session, kiểm ownership trước khi cấp ticket WebSocket/control grant; lần sau login Google tự lấy lại danh sách máy trên điện thoại/PC khác.

Nếu `REQUIRE_DEVICE_INVENTORY=0`, Worker v3.8.1 có thể auto-admit máy mới theo rate-limit và ghi vào `device_inventory`. Nếu đặt `=1`, quay lại chế độ factory allowlist nghiêm ngặt.

## Cấu trúc repo

```text
MAYAP_INDUSTRIAL_v1_0_0/   ESP32 firmware 1.1.0
ATTINY13A_POWER_ALARM/     firmware ATtiny13A
cloudflare/                Worker + D1 migrations
.github/workflows/         CI/build/release/deploy
app.js/config.js/...       Web PWA
release-manifest.json      manifest đồng bộ release
tools/                     regression/sync checker
doc/                       kiến trúc, deploy, commissioning, safety
audit/                     audit lịch sử + delta audit
```

Tên sketch hiện hành được đồng bộ với release 1.0.0: `MAYAP_INDUSTRIAL_v1_0_0/MAYAP_INDUSTRIAL_v1_0_0.ino`. Phiên bản runtime vẫn lấy từ `MAYAP_FIRMWARE_VERSION` và `release-manifest.json`; tên thư mục không được dùng thay cho kiểm tra version trong CI.

## Build ESP32

Board: **ESP32-S3-WROOM-1U-N8**, flash thật 8 MB, không PSRAM.

Thiết lập bắt buộc:

- Flash Size: 8 MB
- Partition Scheme: `default_8MB` / “8M with spiffs”
- PSRAM: disabled
- Không dùng `huge_app` vì cấu hình đó không có dual OTA phù hợp dự án.

CI dùng FQBN:

```text
esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=default_8MB,FlashSize=8M,PSRAM=disabled
```

### Build local và identity

Không cần broker username/password khi build. Mỗi ESP32 có key ngẫu nhiên 256-bit trong NVS; Worker lưu hash có pepper và cấp command key riêng như baseline. Template `build_secrets.h` rỗng chỉ giữ optional LAN OTA password; không có credential fleet. Bench có thể dùng `build_secrets.local.h` đã được gitignore để override host pilot. Không commit key thiết bị hoặc private OTA key.

### Build profile

Workflow hiện chia 3 profile mà không cần sửa source:

- **DEV:** diagnostic Serial ON, input simulation OFF.
- **PILOT:** chạy `workflow_dispatch`, diagnostic Serial ON, input simulation OFF.
- **PROD:** build từ tag `vX.Y.Z`, diagnostic Serial OFF, input simulation OFF.

Tag release phải khớp chính xác `MAYAP_FIRMWARE_VERSION`.

## CI/release invariants

Trước build/release, pipeline bắt buộc:

1. kiểm version/tag;
2. cấm `setInsecure()`;
3. cấm private key trong firmware;
4. kiểm contract ESP32 ↔ ATtiny;
5. chạy `tools/check_release_sync.py`;
6. chạy `tools/check_reliability.py`;
7. syntax-check toàn bộ entrypoint JS, gồm cả security/reliability wrapper;
8. compile ATtiny với giới hạn 1 KB flash / 64 B static RAM;
9. compile ESP32;
10. với tag: ký ECDSA và tạo GitHub Release.

CI dùng Node 24. Arduino CLI được tải từ GitHub Release chính thức ở phiên bản cố định và kiểm SHA-256 trước khi cài, không phụ thuộc `arduino/setup-arduino-cli@v2`.

## Cloudflare

Entrypoint hiện hành là:

```text
cloudflare/src/reliability-wrapper.js
  -> security-wrapper.js
     -> index.js
```

`wrangler.toml` hiện giữ compatibility date `2026-08-01` và `nodejs_compat` có chủ ý. Không tự động đẩy compatibility date theo ngày hiện tại khi chưa regression-test Worker.

Deploy: `.github/workflows/deploy-cloudflare-worker.yml`.

Hiện `cloudflare/` chưa commit `package-lock.json`, vì vậy workflow dùng `npm install` với dependency trực tiếp đã pin và phát warning. Khi lockfile được tạo/commit hợp lệ, workflow tự chuyển sang `npm ci`.

Chi tiết: `cloudflare/README.md` và `doc/DEPLOY_V3_8_1.md`.

## OTA

Có hai đường OTA độc lập:

- **ArduinoOTA LAN:** chỉ dùng khi đặt `MAYAP_OTA_PASSWORD`; để trống là tắt.
- **Internet OTA:** GitHub Release → Worker → ESP32; không tự flash từ xa. Operator phải xác nhận tại máy.

Private signing key chỉ được đặt trong GitHub Actions secret. Firmware chỉ chứa public key xác minh.

## Tài liệu chuẩn

- `doc/ARCHITECTURE_V3_8_1.md` — source-of-truth kiến trúc.
- `doc/DEPLOY_V3_8_1.md` — triển khai hiện hành.
- `doc/COMMISSIONING_V3_8_1.md` — checklist máy pilot và soak.
- `doc/SAFETY_HARDWARE_REQUIREMENTS.md` — yêu cầu an toàn phần cứng.
- `doc/DEPLOY_V3_8_0.md` — **archive, không dùng để triển khai**.
- `audit/06_V381_DELTA_AUDIT.md` — trạng thái delta audit v3.8.1.

## Nguyên tắc source-of-truth

Khi comment/tài liệu cũ mâu thuẫn với code thực thi, phải xác minh lại và sửa tài liệu; không dùng comment cũ để thay đổi hành vi an toàn đang chạy. Với heater safety, provisioning, WebSocket auth, ATtiny và OTA, mọi thay đổi phải đi qua regression gate và commissioning trước khi phát hành hàng loạt.
