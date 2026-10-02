# Yêu cầu an toàn nhiệt bắt buộc cho MAYAP v3.8.x

## Phạm vi

Firmware chỉ là một lớp bảo vệ. Không được dùng ESP32, cảm biến chính hoặc SSR
làm lớp cắt nhiệt duy nhất.

## Chuỗi công suất bắt buộc

Nguồn heater phải đi tuần tự qua:

1. Cầu chì nhiệt dùng một lần.
2. Thermostat/rơ-le nhiệt cơ độc lập với ESP32.
3. Contactor an toàn.
4. SSR điều khiển công suất.
5. Điện trở gia nhiệt.

Thermostat phải có cảm biến riêng và tiếp điểm phải cắt trực tiếp cuộn contactor.
Không đưa thermostat qua phần mềm rồi mới quyết định cắt.

## Hành vi phần mềm hiện hành

- Mất, sai hoặc nghi ngờ cảm biến: SSR OFF và contactor nhiệt OFF.
- Sensor Frozen không được kết luận chỉ vì PV đứng yên ở điểm đặt. Firmware chỉ
  kích hoạt lỗi khi cảm biến đứng giá đủ lâu **và** trong cùng thời gian heater đã
  tích lũy một lượng ON-time đáng kể nhưng PV vẫn không dịch chuyển. Khi điều kiện
  này được xác nhận, fault ở mức STOP và cắt cả SSR lẫn contactor nhiệt.
- Nhiệt độ cao: SSR OFF, contactor OFF, quạt thông gió và tuần hoàn ON.
- Nhiệt khẩn cấp: cắt tức thời, còi và khóa vận hành theo logic hiện có.
- Công tắc nhiệt vật lý chỉ là điều kiện cho phép bật; không thể vượt qua fault.

Mục tiêu của logic Sensor Frozen là tránh false-trip khi buồng thật sự ổn định,
nhưng vẫn bắt được tình huống nguy hiểm "cảm biến kẹt ở mức thấp trong khi heater
vẫn đang cấp nhiệt". Nếu sau này bổ sung cảm biến nhiệt độc lập thứ hai, có thể
nâng cấp chẩn đoán bằng so sánh chéo hai cảm biến nhưng không được bỏ lớp
thermostat cơ độc lập.

## Kiểm thử nghiệm thu máy thật

- Chập SSR ở trạng thái dẫn: thermostat độc lập phải cắt contactor.
- Rút cảm biến: contactor phải nhả.
- Chập cảm biến: contactor phải nhả.
- Giả lập Sensor Frozen khi heater thực sự đang ON đủ lâu: SSR và contactor phải
  bị cắt; ngược lại, PV đứng yên ở điểm đặt khi heater gần như không cấp không
  được gây false-trip.
- Ép nhiệt vượt ngưỡng High: contactor phải nhả và hai quạt chạy.
- Treo/reset ESP32: đầu ra phải về trạng thái an toàn.
- Dính contactor: cầu chì nhiệt/thermostat cấp cao hơn phải ngắt nguồn heater.

Không được xuất xưởng nếu bất kỳ phép thử nào ở trên thất bại.
