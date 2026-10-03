# Browser dependencies

Thư mục này chỉ chứa dependency browser được vendored có chủ ý.

- `jsQR.min.js`: fallback giải mã QR cho browser không có `BarcodeDetector` (đặc biệt WebKit/iOS).
- Realtime **không dùng MQTT.js**. Browser dùng native `WebSocket` thông qua bounded client first-party `realtime_transport.js`.

MQTT.js/bundle đã được loại khỏi runtime sau khi migration WebSocket/DeviceHub và regression tương ứng đạt yêu cầu.

Không thêm CDN runtime tùy ý vào đây. Dependency mới phải được review về license, kích thước, CSP/offline behavior và phải đi qua Web regression trước khi đưa vào public asset allowlist.
