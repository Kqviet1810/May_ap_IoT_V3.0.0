# MAYAP 4.0.0 — audit và chạy thử

## Thay đổi

- Web hướng dẫn đổi Wi-Fi từ HMI: Cài đặt chung → Hệ thống → Đổi Wi-Fi.
- ATtiny13A dùng bản PowerAlarm PROD v2.1 LINKFIX do người dùng đã thử; không
  đổi logic. Thêm metadata protocol v4 và entry point cho CI AVR thuần.
- MQTT handshake, Cloud HTTPS và OTA HTTPS dùng chung admission gate không
  chặn control task. Không khởi tạo đồng thời ba working set TLS; thiếu ngân
  sách heap thì hoãn và giữ retry/request. MQTT đã kết nối vẫn được xử lý.
- PID giữ anti-windup, derivative trên PV và chuyển cấu hình bumpless; thêm
  lọc nhánh D 5 giây và xử lý PV/setpoint không hữu hạn.
- Auto tune bỏ chu kỳ quá độ đầu; dùng ba chu kỳ có biên độ/chu kỳ lệch không
  quá 20% so với trung bình. Nếu chưa ổn định thì tiếp tục cửa sổ trượt đến
  timeout; không ghi tham số từ dao động chưa ổn định. Giới hạn gain đồng bộ
  theo tỷ lệ để không phá Ti/Td. Thành công chuyển sang PID, lưu/readback;
  lỗi timeout/mode/safety tắt nhiệt và hậu làm mát.
- HMI “Kiểm tra cập nhật” thực sự gửi yêu cầu kiểm tra khi chưa có metadata.
  Web vẫn chỉ yêu cầu kiểm tra, không tự flash; cần xác nhận vật lý trên HMI.
- OTA/rollback kiểm tra máy rảnh, không mẻ/resume/test/tune, không nguồn
  nhiệt/động cơ đảo hoặc quá nhiệt. Trong OTA Internet, nhiệt/đảo bị khóa,
  quạt/cảnh báo an toàn vẫn hoạt động. Tải giới hạn đúng số byte còn lại,
  xác minh checksum và ECDSA trước Update.end(false).
- Cổng ArduinoOTA LAN chỉ mở khi máy rảnh và nguồn nhiệt đã tắt. Upload
  đang chạy vẫn được phục vụ đến hết; lỗi trả máy khỏi trạng thái bảo trì.
- build_secrets.h có sẵn, rỗng trên Git; không cần đổi tên. Tuyệt đối không
  push file đã điền thông tin thật. Tag v4.0.0 tạo binary ký bằng GitHub Secrets.

## Panic: kết luận có giới hạn

Chưa có Guru Meditation/backtrace nên KHÔNG xác định được nguyên nhân reset
đã xảy ra, cũng không thể khẳng định đã chữa hết panic. Rà mã thấy nguy cơ
working set TLS đồng thời trên ESP32 không PSRAM; admission gate giảm nguy
cơ này. Giữ WDT/supervisor/brownout và E401, không vô hiệu để che lỗi.
Diagnostic stack log bổ sung OTA; Release giữ ELF để giải mã backtrace cùng
đúng binary. Các nhánh ISR bus dùng timestamp và mảng giới hạn, không cấp
phát/logging trong ISR; chưa thấy bằng chứng tràn mảng tại đó.

## Giới hạn PID và ATtiny

Đây là PID có các đặc tính thực dụng cho điều khiển nhiệt, KHÔNG phải chứng
nhận chuẩn công nghiệp/functional safety. Tự tune relay còn phụ thuộc quán
tính, công suất, tải, sensor và trễ nhiệt thật. Test host dùng chính lớp C++
trong firmware nhưng mô hình nhiệt chỉ là mô hình kiểm thử, không thay máy.
ATtiny đã compile cho attiny13a @1.2 MHz, 874 byte Flash/2 byte RAM tĩnh;
RAM này chưa bao gồm stack. Kiểm contract/dải decode/parity không chứng minh
được clock RC, mức điện áp GPIO, pin sense và relay trên PCB thật.

## Checklist trước mẻ thật

1. Khi máy rảnh, lưu lại cấu hình hiện tại; tắt công tắc nguồn nhiệt/đảo.
2. Cập nhật bản ký từ HMI; kiểm tra báo đúng 4.0.0, config/mẻ không bị xóa.
3. Thử ngắt mạng giữa OTA: máy phải giữ firmware cũ, báo thất bại; không
   chọn boot vào ảnh tải dở. Thử chữ ký sai/checksum sai trong môi trường test.
4. Thử rollback với ảnh trước đó hợp lệ; không dùng phần vùng đã bị ghi dở.
5. Thử Wi-Fi từ HMI, đổi thành công và hết thời gian chờ, kiểm tra kết nối lại.
6. Chạy auto tune KHÔNG có trứng; theo dõi nhiệt đỉnh và timeout. Kiểm tra
   công tắc AUTO/nhiệt, sensor và lỗi storage trong lúc tune đều hủy an toàn.
7. Chạy mẻ giả với tải tương đương thật đủ lâu, theo dõi vượt lố/ổn định và RAM.
8. Thử ATtiny: 7 lệnh, trạng thái/parity, rút BUS phải E501; mất 3V3 khi
   Batch/activity armed phải bật còi, không armed thì không; mất 9V phải E502.
9. Giữ thermostat/thermal fuse độc lập. Firmware không thay lớp bảo vệ cứng.
