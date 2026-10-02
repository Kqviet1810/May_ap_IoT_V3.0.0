# 06 — v3.8.1 delta audit / release synchronization

## Trạng thái

```text
Baseline audit history: v3.7.0
Current target:         v3.8.1 reliability branch
Static synchronization: implemented
Hardware runtime audit: NOT CLAIMED by this document
```

Tài liệu audit v3.7.0 được giữ nguyên như historical evidence; không sửa header cũ để giả vờ nó đã audit v3.8.1.

## Delta được harden ở v3.8.x

- Control/network task separation và supervisor trip.
- Heater fault descriptors/output arbiter.
- ATtiny protocol contract CI.
- Device identity/device key persist fail-closed.
- Browser session + PIN rate-limit + per-device browser limit.
- Reliability wrapper cho provisioning/self-heal.
- MQTT write HMAC theo device.
- TLS fail-closed, cấm `setInsecure()`.
- Internet OTA với hash/signature/operator confirmation.
- Web service-worker update strategy.
- Release sync manifest/gate.
- Node 24 CI migration và loại one-shot hardening path.

## Release gates hiện hành

`tools/check_release_sync.py` chặn drift giữa release manifest, firmware/HMI/web/ATtiny/toolchain/workflow.

`tools/check_v381_reliability.py` chặn regression các invariant v3.8.1.

Build/release workflow phải chạy cả hai checker trước compile/release. Worker deploy workflow cũng chạy cả hai checker trước migration/deploy.

## Những gì vẫn cần hardware evidence

Các mục sau không thể kết luận chỉ bằng static review:

- PID/thermal dynamics ở chamber thật.
- noise/EMI khi SSR, relay, contactor vận hành.
- brownout/power-loss timing trên nguồn thật.
- RS485 sensor fault behavior thực tế.
- watchdog/deadline dưới network stall thật.
- OTA interruption ở mạng yếu.
- SSR stuck/contact welded và lớp thermostat/thermal fuse độc lập.

Bằng chứng chấp nhận phải theo `doc/COMMISSIONING_V3_8_1.md`, đặc biệt soak 72 giờ.

## Known synchronization debt

`cloudflare/package-lock.json` chưa tồn tại. Deploy workflow đã sẵn sàng dùng `npm ci` ngay khi lockfile hợp lệ được commit; cho tới lúc đó nó phát warning và dùng dependency trực tiếp đã pin bằng `npm install --package-lock=false`.

Không coi mục này là lỗi runtime hiện tại, nhưng đây là dependency reproducibility debt còn mở.
