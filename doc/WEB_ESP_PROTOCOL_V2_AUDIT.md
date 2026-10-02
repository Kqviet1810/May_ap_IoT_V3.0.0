# Audit giao tiếp Web ↔ ESP32: giao thức V2

## Phạm vi và luồng trước khi sửa

Web `app.js` gọi Worker `/api/device/sign-mqtt` trước từng command, config,
reminder và history; Worker ký HMAC dài hạn; ESP kiểm tra envelope V1 rồi đưa
command vào `queueCommand` hoặc config vào `startConfigSave`. `mqttTask` 20 ms và
Cloud HTTPS task riêng đã tồn tại. Các thao tác an toàn và EEPROM vẫn do
`machine_control.h` thực hiện.

Các lỗi có thật: Web coi `accepted` là kết quả thành công cho command; Save
chờ thêm `config/reported` dù controller đã xác nhận EEPROM; config gửi toàn
bộ mỗi lần; Web subscribe tất cả topic cho mọi máy và sync trước SUBACK; phiên
realtime ESP chỉ có một boolean; giới hạn gói 1350/1450/1536 nằm ở nhiều lớp.

## Luồng V2

1. Worker xác thực browser session và cấp grant 5 phút cùng khóa phiên dẫn
   xuất từ command key của thiết bị. Browser giữ khóa phiên trong RAM, ký
   `device/channel/grant/body` qua WebCrypto; long-term key không rời Worker/ESP.
2. Browser publish QoS1 trực tiếp. ESP xác minh grant, hạn dùng theo đồng hồ,
   HMAC, clientId, nonce và sequence. Mỗi client có replay fence RAM. ESP trả
   ACK `received`, controller thực hiện, rồi ACK `completed` với `ok`, `code`,
   `message`, `revision`, thời điểm nhận/hoàn thành. ACK được ký bằng khóa
   phiên của transaction; Web kiểm chữ ký trước khi cập nhật UI.
3. RAM cache 16 kết quả cuối ngăn chạy lại cùng requestId. Web thử gửi lại
   đúng envelope/requestId một lần khi thiếu ACK. Hết hạn chờ là `UNCERTAIN`,
   kèm yêu cầu sync; trạng thái máy chỉ giúp đối chiếu, không quy kết nhầm
   thao tác là thành công. Sau terminal ESP ép snapshot vòng MQTT tiếp theo.
4. Config gửi PATCH của form; ESP copy cấu hình đã biết, kiểm kiểu/field,
   sanitize và kiểm tra invariant, sau đó đi qua save/readback EEPROM của
   controller. `applied` chỉ phát sau verify. Full config report chia phần
   dưới 1 KB, dùng để đối chiếu nền.
5. Web chờ SUBACK rồi sync máy đang chọn; máy khác chỉ subscribe presence.
   ESP dùng 8 lease theo clientId để hai tab không tắt realtime của nhau.

Firmware V1 không nhận control từ Web V2: UI yêu cầu cập nhật firmware. Worker
V2 phải triển khai trước khi Web V2; Worker giữ `/sign-mqtt` chỉ cho các trang
V1 đang mở trong giai đoạn chuyển tiếp. Web V2 không gọi endpoint đó.
PID, heater, turning, alarm, HMI, bố cục
EEPROM và AT24C32 history không bị thay đổi đường điều khiển.

## Ngân sách và kiểm tra

| Mục | Giới hạn | Mẫu worst-case trong CI |
| --- | ---: | ---: |
| MQTT transport | 4096 B | buffer 4096 B |
| Gói ứng dụng | 2048 B | bị chặn cả Web/ESP |
| Command V2 | mục tiêu 512 B | 497 B (kể topic/header) |
| PATCH form nâng cao | mục tiêu 1024 B | 958 B (kể topic/header) |
| Config report/history/snapshot | mục tiêu 1024 B | test fail nếu vượt |
| ACK V2 có HMAC | mục tiêu 512 B | test fail nếu vượt |

CI chạy Node transaction/packet tests, static reliability checks, JavaScript
syntax và firmware Arduino CLI cho ESP32-S3/ATtiny. Các bài Node mô phỏng ACK
mất/đảo thứ tự, reject, hai browser, reboot, expired/invalid auth; chúng không
thay thế thử nghiệm broker, EEPROM lỗi và máy vật lý.

## Gate thử máy trước merge

- Worker V2 được deploy trên môi trường thử trước; dùng `workflow_dispatch`
  với `source_ref=feature/web-esp-transaction-v2`.
- Build firmware test bằng `workflow_dispatch` trên nhánh feature để dùng
  MQTT secrets thật. PR build dùng credential giả **chỉ để compile**, không
  flash file đó. Flash bản thử lên một máy thử, sao lưu cấu hình trước.
- Kiểm tra hai browser đồng thời; mất mạng/reconnect; Web SUBACK → sync;
  đo tCreated/tPublished/tDeviceReceived/tDeviceCompleted/tStateReceived.
- Thử toàn bộ command, config, reminder và history; cố tình tắt AUTO, heater,
  gây fault/sensor/RTC phù hợp môi trường thử để xác nhận mã từ chối; thử
  EEPROM không sẵn sàng và khởi động lại giữa transaction.
- So sánh PID/heater/turning/alarm/HMI và history AT24C32 với bản gốc trong
  thử nghiệm an toàn. Chỉ merge khi các gate này và CI đều đạt.
