import { buildPushPayload } from '@block65/webcrypto-web-push';

// Gui 1 thong bao Web Push toi 1 subscription. Tra ve:
//  { ok: true }                       - gui thanh cong
//  { ok: false, gone: true }          - subscription het han/bi thu hoi (404/410) -> nen xoa khoi DB
//  { ok: false, gone: false, status } - loi khac (mang, 5xx tu push service...) - KHONG xoa, co the thu lai sau
export async function sendWebPush(env, subscriptionRow, notification) {
  const vapid = {
    subject: env.VAPID_SUBJECT,
    publicKey: env.VAPID_PUBLIC_KEY,
    privateKey: env.VAPID_PRIVATE_KEY,
  };

  const subscription = {
    endpoint: subscriptionRow.endpoint,
    expirationTime: null,
    keys: {
      p256dh: subscriptionRow.p256dh,
      auth: subscriptionRow.auth,
    },
  };

  const message = {
    data: JSON.stringify(notification),
    options: {
      ttl: 3600, // 1h la du: canh bao cu hon 1h khong con y nghia gui tiep
    },
  };

  // wrangler tail (dang tail mac dinh) chi hien HTTP status Worker tra ve cho
  // TRINH DUYET (luon la 200, xem handleTestPush/handleAlarm) - KHONG hien
  // duoc ket qua goi fetch() toi may chu push (Apple/Google/Mozilla) o day
  // neu khong tu console.log/error ra. Day la ly do "Ok" tren tail truoc do
  // khong phan anh dung viec gui that toi Apple co thanh cong hay khong.
  const endpointHost = (() => { try { return new URL(subscription.endpoint).host; } catch (_) { return '?'; } })();
  try {
    const payload = await buildPushPayload(message, subscription, vapid);
    const res = await fetch(subscription.endpoint, payload);
    if (res.ok) {
      console.log(`[push] OK host=${endpointHost} status=${res.status}`);
      return { ok: true };
    }
    const rawBody = await res.text().catch(() => '');
    const body = rawBody.slice(0, 300);
    console.error(`[push] FAIL host=${endpointHost} status=${res.status} body=${body}`);
    if (res.status === 404 || res.status === 410) return { ok: false, gone: true, status: res.status, error: body };
    return { ok: false, gone: false, status: res.status, error: body };
  } catch (error) {
    console.error(`[push] THROW host=${endpointHost} error=${String(error && error.message ? error.message : error)}`);
    return { ok: false, gone: false, status: 0, error: String(error && error.message ? error.message : error) };
  }
}

// alarm_type -> khong con dung de suy ra noi dung (ESP32 da gui san "message"
// tieng Viet da format san, tai dung faultSummaryText() giong nhu luc con
// Telegram) - o day chi quyet dinh TIEU DE + emoji theo severity/state, tranh
// lap lai bang tra cuu ma loi o ca 3 noi (ESP32/Worker/frontend).
export function buildNotificationPayload({ deviceId, deviceName, alarmType, severity, state, message, temperature, humidity }) {
  let emoji;
  if (state === 'resolved') emoji = '🟢';
  else if (severity === 'critical') emoji = '🔴';
  else if (severity === 'warning') emoji = '🟠';
  else if (severity === 'system') emoji = '⚙️';
  else emoji = '🔵';

  // TEN MAY luon nam trong TIEU DE (khong phai trong body) - tieu de thong
  // bao hau nhu khong bao gio bi cat bot tren Android/iOS, khac voi body de
  // bi rut gon "..." khi qua dai. Nguoi dung theo doi NHIEU may tren cung 1
  // dien thoai can biet NGAY may nao bao ma khong phai mo rong thong bao.
  const title = `${emoji} ${deviceName || deviceId || 'MAYAP'}`;

  // Body: 1 dong duy nhat, uu tien noi dung quan trong nhat len DAU cau (phan
  // de bi cat khi thu gon la CUOI cau, khong phai dau) - bo dong trong thua
  // thai truoc day, gop nhiet do/am vao cuoi trong ngoac cho gon.
  const readings = [];
  if (Number.isFinite(temperature)) readings.push(`${temperature.toFixed(1)}°C`);
  if (Number.isFinite(humidity)) readings.push(`${Math.round(humidity)}%`);
  const suffix = readings.length ? ` (${readings.join(', ')})` : '';

  return {
    title,
    body: `${message || ''}${suffix}`.trim(),
    // Duong dan TUONG DOI (khong co "/" o dau): trang GitHub Pages nam trong
    // 1 thu muc con (vi du /May_ap_trung_V2.1.1/), Service Worker resolve lai
    // theo self.location.href cua chinh no (xem notificationclick trong
    // sw.js) - dung "/..." tuyet doi se roi ve GOC domain (trang rong/404)
    // thay vi dung trang, day chinh la loi "bam vao thong bao ra trang trong".
    icon: './icons/icon-192.png',
    badge: './icons/badge-72.png',
    data: {
      deviceId,
      deviceName: deviceName || deviceId,
      alarmType,
      severity,
      state,
      // Mo dung trang dang dieu khien VA tu chon dung thiet bi (xem
      // app.js::applyDeepLinkDevice), khong phai trang goc chung chung.
      url: `./?device=${encodeURIComponent(deviceId || '')}`,
      ts: Date.now(),
    },
  };
}
