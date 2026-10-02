# Ghi chú — bản UI/UX

Bong bóng Ghi chú dùng lại token MAYAP và dialog xác nhận hiện có. Nó được
khởi tạo sau khi đăng nhập; không thay đổi transport, firmware hay cấu hình máy.

## Thử giao diện

Bấm bong bóng → **Thử giao diện · không lưu vào máy**. Bản thử bắt đầu rỗng,
cho phép tạo/sửa/xóa, xem tất cả, tìm kiếm và lọc. Ghi chú thử chỉ nằm trong RAM
của trang, tách theo máy đang chọn. Reload sẽ mất; giao diện luôn ghi rõ điều này.
Không dùng bản thử để lưu nhật ký vận hành thật.

Chỉ vị trí bong bóng được lưu vào `localStorage` (`mayap.notes.position.v1`).
Sau kéo, nút hút về cạnh gần nhất và tránh navigation/control ở cạnh đó.
Panel dùng scroll nội bộ, tự căn theo `visualViewport`; nội dung người dùng
được đưa vào `textContent`/`value`, không đưa vào HTML.

“Mẻ hiện tại” chỉ được chọn mặc định khi runtime thật báo `batchRunning`.
Khi không có mẻ, mặc định “Máy / bảo trì” và khóa lựa chọn mẻ cho ghi chú mới.
Sửa giữ ID/thời gian tạo; đóng hoặc hủy form đã thay đổi phải xác nhận.

## AT24C512

Repo hiện chưa có vùng ghi Ghi chú hoặc API Web cho dữ liệu đó. AT24C512 đang
được firmware quản lý cho cấu hình/mẻ/nhắc nhở/lịch sử. Không tái sử dụng vùng
nhắc nhở hoặc lịch sử để ghi dữ liệu mới. Bản UI này không đọc/ghi EEPROM,
không phát lệnh điều khiển và không giả lập xác nhận lưu từ ESP32.

UI hiển thị trạng thái chưa hỗ trợ lưu trên máy, thay vì báo lưu thành công.
Lưu thật trên AT24C512 cần một phạm vi firmware/API riêng được người dùng
chấp thuận, do yêu cầu giai đoạn này cấm thay đổi EEPROM/firmware.

## Kiểm tra

```sh
python3 tools/build_web_assets.py
# Dùng một terminal khác, hoặc HTTP server đã chạy:
python3 -m http.server 8765 --bind 127.0.0.1
# Sau đó:
MAYAP_CHROME=/usr/bin/chromium node tools/test_notes_browser.cjs /tmp/mayap-notes-qa
```

Test dùng account/realtime giả lập của Web QA và trình duyệt Chromium thật,
không kết nối máy. Kiểm tra 1920×1080, 1366×768, 768×1024, 390×844, 390×360,
844×390, kéo chuột/touch, reload vị trí, XSS, CRUD, filter/search, dirty form,
trạng thái loading/error với storage giả, và viewport thu nhỏ mô phỏng bàn phím.
CI chạy test này cùng regression Web hiện có.

Cần kiểm tra thêm bàn phím thật và kéo/tap trên Safari iPhone/Android trước
khi phát hành. Không có thao tác deploy hoặc flash trong bản UI này.
