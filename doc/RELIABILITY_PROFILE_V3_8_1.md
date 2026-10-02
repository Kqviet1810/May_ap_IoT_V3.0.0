# MAYAP v3.8.1 - Reliability Profile cho quy mô hiện tại

## 1. Mục tiêu

Profile này áp dụng cho MAYAP ở quy mô từ vài máy đến vài chục máy, có thể mở
rộng lên vài trăm máy mà không thay đổi kiến trúc điều khiển cốt lõi.

Thứ tự ưu tiên bắt buộc:

1. Safety nhiệt và cơ khí.
2. Ổn định 24/7 và khả năng tự phục hồi.
3. Khả năng vận hành khi Internet/Cloud/MQTT lỗi.
4. Chẩn đoán dễ hiểu tại HMI và Serial.
5. Bảo trì/cập nhật dài hạn.
6. Bảo mật đủ dùng theo rủi ro thực tế, không thêm cơ chế enterprise nếu không
   tạo ra lợi ích vận hành tương xứng.

## 2. Nguyên tắc kiến trúc giữ nguyên

- Control Task không thực hiện I/O mạng.
- MQTT/HTTPS lỗi không được ảnh hưởng PID, fault manager hoặc output arbiter.
- HMI/Web chỉ gửi yêu cầu; MachineController vẫn là nơi quyết định cuối cùng.
- Mất Internet không được làm dừng mẻ ấp.
- OTA không tự cài đặt; người vận hành xác nhận tại máy.
- Mọi đường cắt nhiệt bằng firmware chỉ là lớp bổ sung cho thermostat/cầu chì
  nhiệt độc lập.

## 3. Provisioning nhiều máy

### Chế độ mặc định ở quy mô hiện tại

`REQUIRE_DEVICE_INVENTORY = "0"`

Máy mới:

1. Sinh Device Key 256-bit và lưu NVS.
2. Kết nối Cloudflare qua TLS.
3. `reliability-wrapper.js` kiểm tra định dạng ID/key.
4. Nếu ID đã tồn tại, đi thẳng qua security-wrapper để kiểm tra Device Key cũ.
5. Nếu ID mới và chưa có inventory, admission được rate-limit rồi tự thêm vào
   `device_inventory`.
6. Request tiếp tục đi qua `security-wrapper.js`, sau đó core Worker mới tạo
   record `devices`, command key và Web PIN.

Một record `device_inventory.enabled=0` luôn có quyền khóa ID kể cả khi auto
provisioning đang bật.

### Chế độ factory nghiêm ngặt sau này

Chỉ cần đổi:

`REQUIRE_DEVICE_INVENTORY = "1"`

Khi đó máy mới lại bắt buộc phải có trong inventory như thiết kế v3.8.0.

### Chống spam provisioning

- `MAX_NEW_DEVICE_REGISTRATIONS_PER_HOUR` đặt số admission tối đa trong một cửa
  sổ đối với nguồn tạo máy mới.
- `NEW_DEVICE_REGISTRATION_WINDOW_MINUTES` đặt độ dài cửa sổ; mặc định 60 phút.
- Máy đã đăng ký và reboot/reconnect không bị tính như thiết bị mới.
- Đây là hàng rào chống lạm dụng nhẹ cho quy mô hiện tại; không thay thế factory
  allowlist nếu sau này threat model thay đổi.

## 4. Reset PIN self-heal

Nếu một ESP32 thật đang ở trạng thái chưa từng tạo được record `devices`, thao
tác Reset PIN vật lý tại HMI được phép:

1. Auto-admit theo cùng policy/rate-limit.
2. Register thiết bị bằng Device ID + Device Key đang nằm trong firmware.
3. Nhận Web PIN mới.
4. Trả PIN về ESP32 ngay trong response reset.

Nếu `devices` đã tồn tại nhưng Device Key không khớp, không tự sửa hoặc ghi đè
khóa server; request vẫn bị 401 để tránh chiếm lại một Device ID cũ.

## 5. Chẩn đoán provisioning tại HMI

Khi chưa có Web PIN, firmware phân biệt tối thiểu:

- `DANG DONG BO`: đang thử register.
- `CLOUD OFF`: Wi-Fi/đường mạng Cloud chưa sẵn sàng.
- `TLS ERROR`: lớp TLS/CA không khởi tạo được.
- `SERVER 403`: server từ chối admission/provisioning.
- `KEY ERROR`: Device ID đã tồn tại nhưng Device Key không khớp.
- `CLOUD ERROR`: lỗi HTTP/HTTPS khác.

Sau khi có PIN thật, HMI ưu tiên hiển thị PIN. Máy cũ đã provision nhưng Worker
không phát lại PIN sẽ hiển thị `DUNG PIN CU` như trước.

## 6. Safety consistency

Executable descriptor table là nguồn sự thật cho hành vi fault.

- Sensor Lost / Invalid / Suspect: inhibit SSR + drop heater master.
- Sensor Frozen: chỉ active khi PV đứng giá đủ lâu và heater đã có ON-time đáng
  kể trong cùng cửa sổ; khi active là STOP fault và cắt SSR + heater master.
- High Temperature: cắt SSR + heater master, ép quạt theo policy hiện hành.
- Emergency Temperature: cắt tức thời và khóa theo policy emergency.

Comment/tài liệu không được mô tả khác với các bit `inhibitSsr` và
`dropHeatMaster` trong `FaultDescriptor`.

## 7. Release gate

Một build được coi là ứng viên chạy máy thật chỉ khi:

- Build ESP32 thành công với 8MB/default_8MB/PSRAM disabled.
- Build ATtiny13A thành công và không vượt flash/RAM budget.
- `Reliability regression checks` xanh.
- Không có `setInsecure()` trong firmware.
- Không có private key trong source firmware.
- Protocol ESP32 <-> ATtiny contract xanh.
- Test commissioning P0 hoàn tất.

## 8. Những thứ chủ động KHÔNG thêm ở quy mô hiện tại

- TPM/Secure Element.
- mTLS certificate riêng từng máy.
- MFA/OAuth/SSO.
- RBAC nhiều cấp.
- MQTT HA cluster tự vận hành.
- ESP32 dự phòng/lock-step MCU.
- Persistent telemetry mỗi giây.
- Flash event logger đồng bộ trong Control Task.

Các mục trên chỉ được xem xét lại khi threat model, quy mô hoặc yêu cầu pháp lý
thay đổi.

## 9. Hướng nâng cấp tiếp theo sau v3.8.1

Persistent fault journal chỉ nên được bật sau khi có logger bất đồng bộ đã
bench-test. Không đưa filesystem/flash write định kỳ trở lại Control Task chỉ để
có nhiều log hơn.
