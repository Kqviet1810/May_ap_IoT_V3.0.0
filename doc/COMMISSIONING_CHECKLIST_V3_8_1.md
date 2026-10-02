# MAYAP v3.8.1 - Commissioning / Soak checklist

Checklist này dành cho từng máy trước khi bàn giao hoặc trước khi đưa một bản
firmware mới vào chạy mẻ thật.

## A. Nhận dạng và firmware

- [ ] ESP32-S3-WROOM-1U-N8 đúng phần cứng.
- [ ] Flash Size = 8MB.
- [ ] Partition Scheme = default_8MB / 8M with spiffs, có 2 khe OTA.
- [ ] PSRAM disabled.
- [ ] Firmware báo đúng version dự kiến.
- [ ] Device ID `MAP-XXXXXXXXXXXX` khác các máy khác.
- [ ] Device Key tồn tại sau reboot thường.

## B. Chuỗi an toàn nhiệt bắt buộc

- [ ] Cầu chì nhiệt một lần được lắp đúng đường công suất.
- [ ] Thermostat/rơ-le nhiệt cơ có cảm biến độc lập với ESP32.
- [ ] Thermostat cắt trực tiếp cuộn contactor, không đi qua firmware.
- [ ] Contactor cắt được nguồn heater khi SSR bị giả lập dính ON.
- [ ] SSR đúng loại/dòng, heatsink và thông gió đạt yêu cầu.

## C. Sensor / fault injection P0

- [ ] Rút cảm biến -> SSR OFF, heater master OFF, fault hiển thị.
- [ ] Chập/giả lập dữ liệu sensor invalid -> SSR OFF, heater master OFF.
- [ ] High Temperature -> SSR OFF, heater master OFF, quạt theo policy.
- [ ] Emergency Temperature -> cắt ngay, còi hoạt động, lockout đúng.
- [ ] Sensor Frozen giả lập với heater ON-time đủ -> STOP + cắt SSR/master.
- [ ] PV đứng yên gần setpoint khi heater gần như không cấp -> không false-trip
      Sensor Frozen.
- [ ] HeaterNotHeating/E115 test theo policy release hiện hành.

## D. Điều khiển nhiệt

- [ ] PID giữ nhiệt quanh setpoint trong tải thật.
- [ ] SSR time-proportioning chạy, contactor không đóng cắt theo PID window.
- [ ] Quạt tuần hoàn đạt trạng thái ổn định trước khi cho phép heater theo policy.
- [ ] AUTO/MANUAL không bypass fault nhiệt.
- [ ] Công tắc HEATER vật lý chỉ là enable, không override fault.

## E. Đảo trứng

- [ ] Limit LEFT đúng.
- [ ] Limit RIGHT đúng.
- [ ] Không thể bật LEFT và RIGHT đồng thời.
- [ ] Timeout cơ khí tạo đúng fault.
- [ ] Cả hai limit cùng active tạo fault.
- [ ] Mất một lần lịch đảo/turn count được phát hiện theo policy.
- [ ] Test Mode xác nhận cơ khí trước khi gỡ trạng thái kiểm tra bắt buộc.

## F. Wi-Fi / MQTT / Cloud

- [ ] ONLINE kết nối Wi-Fi thành công.
- [ ] MQTT TLS kết nối HiveMQ thành công.
- [ ] Hai máy dùng cùng broker credential nhưng có topic/device ID riêng.
- [ ] Mất MQTT không ảnh hưởng PID/heater control.
- [ ] Mất Cloudflare không ảnh hưởng PID/MQTT.
- [ ] Mất Wi-Fi không ảnh hưởng điều khiển tại máy.
- [ ] Wi-Fi quay lại -> MQTT/Cloud tự reconnect bằng backoff.

## G. Provisioning máy mới

- [ ] Xóa record test của một Device ID mới khỏi `devices` và
      `device_inventory` trước phép thử (chỉ dùng máy test).
- [ ] Bật máy ONLINE.
- [ ] Máy tự register mà không cần INSERT inventory thủ công.
- [ ] HMI nhận Web PIN.
- [ ] `device_inventory` được tạo tự động với `enabled=1`.
- [ ] `devices` được tạo đúng Device ID.
- [ ] Web nhập ID + PIN và ghép thành công.
- [ ] MQTT realtime hoạt động sau pairing.

## H. Provisioning error diagnostics

- [ ] Tắt Wi-Fi -> khi chưa có PIN, HMI hiện `CLOUD OFF`.
- [ ] Worker trả 403 trong strict mode -> HMI hiện `SERVER 403`.
- [ ] Dùng một NVS Device Key khác cho ID đã đăng ký trên máy test -> HMI hiện
      `KEY ERROR` và server không bị ghi đè khóa cũ.
- [ ] TLS/CA cố ý sai trên build test -> HMI hiện `TLS ERROR`; firmware không
      fallback sang TLS insecure.

## I. Reset PIN

- [ ] Máy đã đăng ký: Reset PIN tại HMI sinh PIN mới.
- [ ] Browser session cũ bị revoke theo Worker policy.
- [ ] Máy test chưa có `devices`: Reset PIN tự register + trả PIN mới.
- [ ] ID đã tồn tại nhưng key sai: Reset PIN phải thất bại, không tự chiếm lại ID.

## J. Mất điện / recovery

- [ ] Mất nguồn trong mẻ -> state recovery đúng theo cấu hình.
- [ ] RTC còn hợp lệ -> thời gian mất điện được xử lý đúng.
- [ ] RTC invalid -> không tự suy đoán thời gian, yêu cầu policy xác nhận đúng.
- [ ] ATtiny giữ đúng trạng thái batch.
- [ ] Mất 3V3 ESP32 trong batch -> còi độc lập hoạt động theo thiết kế.
- [ ] Có điện lại -> ESP/ATtiny đồng bộ lại.

## K. Watchdog / restart

- [ ] Giả lập Control Task treo -> Supervisor đưa output safe trước restart.
- [ ] Giả lập HMI fatal -> output safe/restart đúng policy.
- [ ] Brownout/reset bất thường -> reason được ghi nhận và recovery đúng.
- [ ] Health restart chỉ xảy ra tại thời điểm an toàn theo policy.

## L. OTA

- [ ] Check firmware chỉ báo có bản mới, không tự flash.
- [ ] Chỉ bắt đầu OTA sau xác nhận vật lý tại HMI.
- [ ] File đúng SHA/signature -> cài thành công.
- [ ] File sai hash/signature -> `Update.abort()`, firmware cũ tiếp tục chạy.
- [ ] Rút Wi-Fi giữa download -> abort sạch, không boot image dở.
- [ ] Test rollback về khe OTA trước thành công.

## M. Soak test tối thiểu 72 giờ

Trong toàn bộ 72 giờ:

- [ ] Không watchdog/reset ngoài kế hoạch.
- [ ] Heap không có xu hướng giảm liên tục.
- [ ] Không tăng bất thường số lần reconnect MQTT/Cloud khi mạng ổn định.
- [ ] PID không xuất hiện dao động tăng dần.
- [ ] Relay/contactors không vượt rate policy.
- [ ] Sensor không phát false fault lặp lại.
- [ ] Ít nhất 3 lần ngắt/khôi phục Wi-Fi test trong soak.
- [ ] Ít nhất 1 lần reboot có chủ đích và recovery thành công.
- [ ] Ít nhất 1 chu kỳ đảo trái/phải đầy đủ nhiều lần.

## N. Điều kiện không được bàn giao

Không bàn giao nếu một trong các điều kiện sau còn tồn tại:

- Bất kỳ test safety nhiệt P0 thất bại.
- Control bị ảnh hưởng khi mất Internet/MQTT.
- Máy mới không tự provisioning ở profile hiện tại.
- Device Key mismatch có thể ghi đè record cũ.
- OTA sai hash/signature vẫn có thể boot.
- Watchdog/restart không đưa output về safe.
- Có reset không giải thích được trong soak 72 giờ.
