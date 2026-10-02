# Ghi chú trên AT24C512

Bong bóng Ghi chú dùng token MAYAP, dialog xác nhận và scroll nội bộ hiện có.
Chỉ vị trí nút lưu vào localStorage. Nội dung ghi chú được lưu trên máy đang chọn,
không lưu vào D1 hoặc bộ nhớ trình duyệt. Cần flash firmware có `notesVersion=1`;
firmware cũ hiển thị yêu cầu cập nhật, không báo lưu giả.

## Dữ liệu và vùng nhớ

Mỗi máy lưu tối đa 16 ghi chú, tiêu đề 60 ký tự và nội dung 300 ký tự, hỗ trợ
Unicode tiếng Việt. ID và thời gian tạo giữ nguyên khi sửa; phiên bản tăng khi ghi.
“Mẻ hiện tại” chỉ tạo được khi runtime thật báo có mẻ. Ghi chú mẻ cũ vẫn sửa được.
Nội dung hiển thị bằng textContent/value, không chèn vào HTML.

Vùng 0x3000–0xEFFF dành riêng cho ghi chú: 16 slot, mỗi slot hai bank 1536 byte.
Không dịch chuyển hoặc xóa cấu hình, mẻ, nhắc nhở và lịch sử hiện có. Mỗi bản ghi
có schema, sequence và CRC; ghi bank đối diện rồi đọc lại đầy đủ trước xác nhận.
Khởi động chọn bản hợp lệ mới nhất; mất điện giữa chừng giữ bản cũ hợp lệ.
Không format EEPROM lúc boot. Chip hỏng hoặc dữ liệu hỏng được báo lỗi rõ ràng.

Task ghi chú riêng trên Core 0, ưu tiên thấp, queue cố định và dùng driver/khóa
I2C hiện có. Không gọi EEPROM từ ISR hoặc controller. Các lệnh notes/request,
notes/set dùng cơ chế V2 đang có: ownership, HMAC, bootId, seq, expiry, requestId.
Web chỉ báo đã lưu sau terminal ACK của ESP32 và đọc lại thành công. Xung đột
phiên bản yêu cầu tải lại, không ghi đè âm thầm giữa hai trình duyệt.

Nếu kết quả ghi chưa chắc chắn, Web giữ trạng thái chưa xác nhận; owner đọc lại
và đối chiếu mỗi hai giây, không ghi lặp dữ liệu đã commit. Không phát ACK thành
công giả hoặc reset controller. Một lỗi EEPROM chỉ chặn thao tác ghi chú tiếp theo.

## Kiểm tra tự động

```sh
python3 tools/test_notes_store.py
python3 tools/test_runtime_buses.py --sanitize --check-regression
node --test tests/*.test.cjs
python3 tools/build_web_assets.py
MAYAP_CHROME=/usr/bin/chromium node tools/test_notes_browser.cjs /tmp/mayap-notes-qa
```

Store test dùng driver thật và giả lập C512, bao gồm 1505 vị trí mất điện,
write protection, CRC fallback, full, stale version, Unicode, readback không chắc
chắn và bảo toàn vùng EEPROM cũ. Browser test dùng storage/device giả lập có ACK
HMAC; kiểm tra sáu viewport, CRUD, reload, tìm kiếm, XSS, loading/error, drag/touch.
Các test này không thay thế kiểm tra ESP32 và EEPROM thật.

## Kiểm tra máy thật sau flash

- Lưu ghi chú tiếng Việt, chờ xác nhận, reload Web rồi tắt/bật nguồn cả máy và EEPROM.
- Sửa/xóa, kiểm tra dữ liệu sau reboot; thử đủ 16 ghi chú.
- Hai trình duyệt cùng sửa: bản cũ phải bị từ chối, tải lại rồi sửa tiếp.
- Mất Internet lúc lưu: không báo thành công trước ACK; reconnect không tạo bản trùng.
- Mất điện giữa ghi: ghi chú cũ hoặc bản mới hoàn chỉnh còn đọc được.
- Kiểm tra PID, heater/safety, HMI và nhắc nhở vẫn hoạt động khi ghi nhiều ghi chú.
- Kiểm tra bàn phím và kéo/tap bằng Safari iPhone thật.

Không có thao tác flash, OTA hay deploy trong thay đổi này.
