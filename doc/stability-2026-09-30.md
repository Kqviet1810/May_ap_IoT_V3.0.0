# Khac phuc log chay me 30/09/2026

## Nguyen nhan va pham vi

- 111 FRAME_INVALID deu co edges=13: end-gap 30 ms bi ap dung ca khi day
  van LOW, trong khi bit 1 hop le den 40 ms. ESP chi ket thuc khi day HIGH;
  decoder chi chap nhan 12/14 canh. Protocol v4, GPIO41 va firmware Tiny
  LINKFIX khong thay doi. E501/E502/E503 va timeout mat day van giu nguyen.
- minEver=284 quanh MQTT CONNECTION_LOST la can RAM tuc thoi, chua chung
  minh leak dai han. TLS admission MQTT: free >=48 KiB, largest >=24 KiB;
  Cloud/OTA: free >=72 KiB, largest >=24 KiB. Bulk MQTT reports dung cung
  atomic admission, khong cap phat config/history lon trong TLS handshake.
  MQTT pump va terminal ACK van duoc phuc vu. Budget nay la admission,
  KHONG phai bao dam TLS khong the xuong thap hon mot nguong khi chay that.
- Cloud body request/response bi gioi han 1024 byte (ke ca chunked).
  Metadata OTA cung gioi han 1024 byte, firmware binary van stream theo
  logic hash/chu ky cu. Khong in noi dung response provisioning ra Serial.
- Backoff HTTPS/MQTT dung millis SAU I/O, gap Cloud toi thieu 3 s sau khi
  request truoc hoan tat. Deferred vi tai nguyen/gap khong tinh la loi mang.
  Yeu cau reset PIN chua gui do deferred duoc giu lai; KHONG tu replay
  reset PIN neu HTTPS da gui nhung ket qua bi loi/khong chac chan.
- Lap lai cung alarm/cung trang thai duoc gop; active/clear/active van la
  ba su kien. Routine traffic khong duoc day Critical ra khoi outbox.
- Config/reminders/report snapshot gui that bai van duoc thu lai. Event
  log gui cu -> moi, toi da 5 muc/tick, cursor chi tien sau publish thanh
  cong. Day la retry loi publish cuc bo, KHONG bien QoS0 thanh durable ACK.
  Backlog RAM van huu han; nhat ky EEPROM la nguon lich su tai may.
- Wi-Fi giu PERFORMANCE, khong dao MODEM_SLEEP theo loi/lease. Web: LWT
  offline ngay; stale 8..30 s hien DU LIEU CHAM, qua 30 s khoa lenh. Lenh
  trong khoang degraded van can signed ACK/bootId/expiry nhu truoc.
  Broker silence co nguong rieng 90 s, khong duoc nho hon hai keepalive;
  khong dung stale 8 s de reset socket dang song. Web cache 12.1.8.
- Serial dung hang doi tinh ~2 KiB chi trong build diagnostic; supervisor
  ghi tung phan khong cho/delay/flush. Co timestamp, uu tien Fault/Health,
  thong ke dropped/critical/truncated. Khi EXIT, ket thuc dong dang ghi
  truoc thong bao OFF, huy diagnostic chua gui. Firmware PROD van tat log.
- Trang thai dinh ky rut gon PROCESS (T/SV/RH/PID/SSR/fault) va HEAP.
  BUILD bao commit CI + ngay gio compile, khong thay EEPROM schema/version.

## Kiem thu tu dong

`node --test tests/*.test.cjs`

`python tools/test_runtime_buses.py --sanitize --check-regression` (Linux/GCC)

Runtime driver su dung header production, Tiny decoder that; them xung
22..40 ms, 6..16 ms, gap 8..25 ms, preamble toi 75 ms, parity, disconnect,
overflow, stall, rollover. Runtime stability test kiem tra admission,
bulk/TLS loai tru nhau, HTTP oversize/truncated/chunked, Serial nghen/mute,
alarm coalescing/critical priority va retry log tren 5 muc.

Fingerprint safety chi doi nhung muc da review trong stabilityReviewNote;
thermal_control, Tiny, schema, output/fault safety va Web transaction/HMAC
giu nguyen. Khong dung viec cap nhat fingerprint thay cho behavioral tests.

Tich hop nguyen cac sua doi moi tren main: doi soat E503 khi chuyen batch/
activity, subscribe QoS1 moi clean session, cua so Arduino OTA 30 phut mot
luot sau reset vat ly, va tai lieu A5 rev 1.3. Khong ghi de cac thay doi nay.

## Nghiem thu tren may that (bat buoc truoc khi coi la on dinh dai han)

1. Nap ESP; KHONG nap Tiny. Bat LOG, ghi BUILD. Cho khoi dong xong.
2. Chay me 24 gio; moi giai doan phai co PROCESS va HEAP/TASK. Theo doi
   heap sau moi request va sau khi nghi; minEver chi la moc thap lich su.
   Neu minEver lai gan can, gui log TLS/HEAP/TASK de dieu chinh budget;
   khong giam E401/E402 de che loi. Theo doi Cloud deferred de phat hien
   admission qua chat; khong chap nhan Cloud bi hoan vinh vien.
3. Lap bat/tat me, luu config, bat/tat den 50 lan; lenh chi thanh cong
   khi ACK cuoi dung requestId. Khong E501/503 gia theo thao tac.
4. Rut BUS: phai bao E501 trong deadline cu. Cam lai: tu hoi phuc.
   Khong ap dung bo dem debounce moi de che mat ket noi that.
5. Ngat Internet 2 phut: nhiet/dao/HMI van chay; hien offline dung;
   noi lai tu dong, retry ACK/log trong gioi han, Cloud khong spam burst.
6. Tat LOG/EXIT, dong cong Serial; khong diagnostic muon sau OFF.
   Kiem thu alarm mat dien/pin 9 V tren phan cung rieng, khong suy dien
   tu host simulation. Khong tu dong OTA/nap may trong luc dang chay me.

Push main tao artifact DEV cho thu nghiem; KHONG tu tao tag/release ky OTA.
