# MAYAP 3.8.1 — Pilot / commissioning checklist

Checklist này dành cho **máy thật** trước phát hành hàng loạt. Static CI không thay thế các bước này.

## A. Flash và boot

- [ ] ESP32-S3 N8 nhận đúng flash 8 MB.
- [ ] Partition `default_8MB`, dual OTA; không có lỗi `partition ... exceeds flash`.
- [ ] Boot không reset loop.
- [ ] DS3231, EEPROM, HMI, RS485 sensor khởi tạo đúng.
- [ ] ATtiny protocol v2 có ACK/status hợp lệ.

## B. Safe outputs

- [ ] Reset ESP32 khi heater đang demand: output về trạng thái an toàn trong reset/startup.
- [ ] Rút sensor nhiệt: SSR OFF + heat master/contactor OFF theo fault hiện hành.
- [ ] Ép nhiệt cao: SSR OFF + contactor OFF + quạt an toàn theo thiết kế.
- [ ] Emergency threshold: cắt nhiệt tức thời và còi hoạt động.
- [ ] SSR giả lập dính ON: thermostat/thermal relay độc lập phải cắt contactor.
- [ ] Contactor giả lập dính: lớp thermal fuse/thermostat cao hơn vẫn bảo vệ được heater.

## C. Turning

- [ ] LEFT/RIGHT limit đúng cực tính và không đảo logic.
- [ ] AUTO turning đến limit và dừng.
- [ ] Không đến limit trong timeout tạo turn fault/alarm.
- [ ] Manual turn không bypass interlock/fault.
- [ ] Mất nguồn giữa lần đảo không tạo lệnh nguy hiểm khi boot lại.

## D. Batch/recovery

- [ ] Start/stop batch lưu đúng.
- [ ] Mất nguồn trong batch: trạng thái phục hồi theo RTC/journal.
- [ ] ATtiny reset riêng trong batch: batch state fail-safe không bị mất sai.
- [ ] ESP32 reset riêng: không phát sinh còi/turn/heater ngoài ý muốn.

## E. Network/MQTT

- [ ] Online/Offline toggle đúng; Offline không tự bật Wi-Fi/Push.
- [ ] Wi-Fi mất rồi phục hồi không làm treo controlTask.
- [ ] Broker mất rồi phục hồi với backoff, không reset storm.
- [ ] Web telemetry cập nhật.
- [ ] command/config/reminder write hợp lệ được nhận.
- [ ] payload sai/không ký bị từ chối.
- [ ] không có credential/password xuất hiện trong log production.

## F. Cloud/browser

- [ ] register HTTP thành công.
- [ ] Device ID + PIN đủ để thêm máy.
- [ ] không cần nhập WSS/broker/account.
- [ ] sai PIN nhiều lần bị 429 theo policy.
- [ ] session hết hạn/revoke yêu cầu xác thực lại.
- [ ] Push active/resolved hiển thị đúng.
- [ ] restart/OTA có chủ ý không gây offline alert giả trong ngưỡng thiết kế.

## G. OTA

- [ ] firmware release đúng version được phát hiện.
- [ ] Web chỉ yêu cầu check; không tự flash.
- [ ] HMI yêu cầu operator xác nhận.
- [ ] `.bin` đúng SHA + signature được chấp nhận.
- [ ] `.bin` đúng SHA nhưng signature sai bị hủy.
- [ ] mất mạng giữa download không phá partition đang chạy.
- [ ] boot firmware mới thành công và rollback contract còn hoạt động.

## H. Soak 72 giờ

Chạy máy PILOT ít nhất 72 giờ với chu kỳ nhiệt/turn/network thực tế:

- [ ] không watchdog trip bất thường;
- [ ] không heap trend giảm liên tục;
- [ ] không MQTT reconnect storm;
- [ ] không Cloud HTTP storm;
- [ ] không false sensor/turn/heater fault lặp lại;
- [ ] PID ổn định ở tải thật;
- [ ] relay/contactor không nóng bất thường;
- [ ] SSR/heatsink trong giới hạn nhiệt;
- [ ] nguồn 12 V/3.3 V ổn định khi đóng/ngắt tải.

## I. Release decision

Chỉ phát hành hàng loạt khi mọi mục safety bắt buộc đạt. Nếu một test phần cứng độc lập như thermostat cắt contactor thất bại, **không dùng firmware để hợp thức hóa việc bỏ qua lỗi phần cứng**.
