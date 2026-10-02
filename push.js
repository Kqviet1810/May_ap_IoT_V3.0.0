// MAYAP Web Push - thay the hoan toan kenh Telegram cu.
// ESP32 -> Cloudflare Worker -> Web Push -> trinh duyet nay (kha ca khi
// website dang KHONG mo, nho Service Worker dang ky o sw.js).
//
// Module nay chi lo phan "Push Subscription" (dang ky/huy/test) - KHONG dung
// polling gia lap, dung dung chuan Service Worker + Push API + Notification API.
(() => {
  'use strict';

  const STORAGE_KEY = 'mayap.push.v1';

  function cloudApiBase() {
    const base = String(window.MAYAP_WEB_CONFIG?.cloudApiBase || '').replace(/\/+$/, '');
    return base;
  }

  function apiUrl(path) {
    const base = cloudApiBase();
    if (!base) return null;
    return `${base}${path}`;
  }

  function isSupported() {
    return 'serviceWorker' in navigator && 'PushManager' in window && 'Notification' in window;
  }

  // iPadOS 13+ gia lam Mac Safari (UA khong con chua "iPad") nen phai kiem
  // tra them "co cam ung + maxTouchPoints" de phan biet voi Mac that.
  function isIos() {
    const ua = navigator.userAgent || '';
    if (/iphone|ipad|ipod/i.test(ua)) return true;
    return /Macintosh/.test(ua) && navigator.maxTouchPoints > 1;
  }

  function isStandalone() {
    if (window.navigator.standalone === true) return true; // Safari iOS
    return window.matchMedia && window.matchMedia('(display-mode: standalone)').matches;
  }

  // Web Push yeu cau applicationServerKey dang Uint8Array, nhung VAPID public
  // key tra ve tu server la chuoi base64url - can tu chuyen doi (khong co ham
  // dung san trong trinh duyet).
  function urlBase64ToUint8Array(base64String) {
    const padding = '='.repeat((4 - (base64String.length % 4)) % 4);
    const base64 = (base64String + padding).replace(/-/g, '+').replace(/_/g, '/');
    const raw = atob(base64);
    const output = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; ++i) output[i] = raw.charCodeAt(i);
    return output;
  }

  function loadLinked() {
    try { return JSON.parse(localStorage.getItem(STORAGE_KEY) || '{}'); } catch (_) { return {}; }
  }
  function saveLinked(data) {
    try { localStorage.setItem(STORAGE_KEY, JSON.stringify(data)); } catch (_) {}
  }

  async function registerServiceWorker() {
    if (location.protocol === 'file:') throw new Error('Can chay qua HTTPS/http server, khong the la file:// ');
    const registration = await navigator.serviceWorker.register('./sw.js');
    await navigator.serviceWorker.ready;
    return registration;
  }

  async function fetchVapidPublicKey() {
    const url = apiUrl('/api/push/vapid-public-key');
    if (!url) throw new Error('Chua cau hinh cloudApiBase trong config.js');
    const res = await fetch(url);
    if (!res.ok) throw new Error(`Khong lay duoc VAPID public key (HTTP ${res.status})`);
    const data = await res.json();
    if (!data?.publicKey) throw new Error('Server chua cau hinh VAPID_PUBLIC_KEY');
    return data.publicKey;
  }

  // "Khoi dong truoc" dang ky Service Worker + lay VAPID public key NGAY khi
  // trang tai xong (khong doi nguoi dung bam nut). Chuyen requestPermission()
  // len dau tien (xem enable() ben duoi) da giai quyet phan lon van de, nhung
  // Safari/iOS con thuc te bi mat "user activation" ngay ca boi 1 await MANG
  // (goi fetchVapidPublicKey) chen giua luc cap quyen va luc goi subscribe() -
  // nen ca 2 buoc nay cung can duoc lam SAN, de trong enable() chi con
  // Promise.all() tren 2 promise (thuong) DA XONG tu truoc, khong con doi
  // mang giua chung "cap quyen" va "subscribe" nua.
  let warmupPromise = null;
  function warmUp() {
    if (!warmupPromise) {
      warmupPromise = Promise.allSettled([registerServiceWorker(), fetchVapidPublicKey()]);
    }
    return warmupPromise;
  }
  if (isSupported() && cloudApiBase()) window.MayapAccount?.ready.then(account => { if(account) warmUp(); });

  // Goi /api/push/subscribe la thao tac UPSERT re/an toan goi lai nhieu lan -
  // dung ca khi bat thong bao lan dau LAN khi tu "vien lai" link cho mot
  // subscription da co san (vi du sau khi trinh duyet tu xoay subscription o
  // su kien pushsubscriptionchange, xem sw.js) ma khong can nguoi dung bam lai.
  async function linkSubscription(deviceId, subscription) {
    const url = apiUrl('/api/push/subscribe');
    if (!url) throw new Error('Chua cau hinh cloudApiBase trong config.js');
    const res = await fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        device_id: deviceId,
        subscription: subscription.toJSON ? subscription.toJSON() : subscription,
      }),
    });
    const body = await res.json().catch(() => ({}));
    if (!res.ok || !body.success) throw new Error(body.error || `Worker tu choi (HTTP ${res.status})`);
    const linked = loadLinked();
    linked[deviceId] = { endpoint: subscription.endpoint, linkedAt: Date.now() };
    saveLinked(linked);
    const accountDevice=window.MayapAccount?.current?.devices.find(d=>d.device_id===deviceId);
    if(accountDevice) accountDevice.linked_browsers=Math.max(1,Number(accountDevice.linked_browsers || 0));
  }

  function errorText(error) {
    // Bao gom ten loi that (vi du DOMException "NotAllowedError") - can de
    // chan doan Safari/iOS khi thu that bai, khac Chrome/Android o dung loi
    // nay nhung khong bao gio hien ra tren giao dien truoc day.
    const name = error?.name ? `${error.name}: ` : '';
    return `${name}${error?.message || error}`;
  }

  // reason co the la: 'unsupported' | 'ios-needs-install' | 'denied' | 'error'
  //
  // QUAN TRONG cho Safari/iOS: WebKit gan chat "user activation" cua cu cham
  // nguoi dung - Notification.requestPermission() phai la thao tac async DAU
  // TIEN, va cang IT await mang (dang ky Service Worker, goi API lay VAPID
  // key) xen giua luc cap quyen va luc goi pushManager.subscribe() cang tot.
  // Chrome/Android khong bi rang buoc nay nen truoc day test tren Android
  // "chay tot" che mat loi nay tren iPhone. Giai phap: "khoi dong truoc" ca
  // Service Worker lan VAPID key ngay luc trang tai (xem warmUp() o tren),
  // de o day chi con cho 1 Promise.allSettled() DA CO SAN (khong phai doi
  // mang), giu subscribe() sat cu cham nhat co the.
  // deviceIds: mang cac device_id can lien ket (TOAN BO thiet bi dang co tren
  // dashboard nay, khong chi 1 may dang chon) - 1 trinh duyet/dien thoai theo
  // doi N may thi phai nhan canh bao ca N may, khong chi may dang dieu khien.
  async function enable(deviceIds, options = {}) {
    const ids = (Array.isArray(deviceIds) ? deviceIds : [deviceIds]).filter(Boolean);
    if (!ids.length) return { ok: false, reason: 'no-device' };
    if (!isSupported()) return { ok: false, reason: 'unsupported' };
    if (!cloudApiBase()) return { ok: false, reason: 'not-configured' };
    if (isIos() && !isStandalone()) return { ok: false, reason: 'ios-needs-install' };

    if (Notification.permission === 'denied') return { ok: false, reason: 'denied' };
    let permission;
    try {
      permission = await Notification.requestPermission();
    } catch (error) {
      return { ok: false, reason: 'error', error: errorText(error) };
    }
    if (permission !== 'granted') return { ok: false, reason: 'denied' };

    const [registrationResult, publicKeyResult] = await warmUp();
    if (registrationResult.status === 'rejected') {
      return { ok: false, reason: 'error', error: errorText(registrationResult.reason) };
    }
    if (publicKeyResult.status === 'rejected') {
      return { ok: false, reason: 'error', error: errorText(publicKeyResult.reason) };
    }

    try {
      const registration = registrationResult.value;
      const publicKey = publicKeyResult.value;
      let subscription = await registration.pushManager.getSubscription();
      if (!subscription) {
        subscription = await registration.pushManager.subscribe({
          userVisibleOnly: true,
          applicationServerKey: urlBase64ToUint8Array(publicKey),
        });
      }

      // Lien ket LAN LUOT tung device_id vao CUNG 1 subscription - loi 1 may
      // (vi du device_key sai/chua dang ky) khong chan cac may con lai.
      let lastError = null;
      let linkedAny = false;
      for (const id of ids) {
        try {
          await linkSubscription(id, subscription, options.pairingTokens?.[id] || '');
          linkedAny = true;
        } catch (error) {
          lastError = error;
        }
      }
      if (!linkedAny) return { ok: false, reason: 'error', error: errorText(lastError) };
      return { ok: true, partial: Boolean(lastError) };
    } catch (error) {
      return { ok: false, reason: 'error', error: errorText(error) };
    }
  }

  // Tat thong bao cho CA TRINH DUYET (khong con rieng tung may) - dung 1
  // subscription duy nhat cho toan bo may da lien ket tren dien thoai nay.
  async function disable() {
    if (!isSupported()) return { ok: true };
    try {
      const registration = await navigator.serviceWorker.getRegistration('./sw.js');
      const subscription = await registration?.pushManager.getSubscription();
      if (subscription) {
        const url = apiUrl('/api/push/subscribe');
        if (url) {
          await fetch(url, {
            method: 'DELETE',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ endpoint: subscription.endpoint }),
          }).catch(() => {});
        }
        await subscription.unsubscribe();
      }
    } catch (_) {
      // Khong chan UI neu huy that bai - trang thai se tu dong bo lai o lan kiem tra tiep theo.
    }
    saveLinked({});
    return { ok: true };
  }

  async function currentSubscriptionEndpoint() {
    if (!isSupported()) return '';
    const registration = await navigator.serviceWorker.getRegistration('./sw.js');
    const subscription = await registration?.pushManager.getSubscription();
    return subscription?.endpoint || '';
  }

  async function testNotification() {
    const endpoint = await currentSubscriptionEndpoint();
    if (!endpoint) return { ok: false, reason: 'not-subscribed' };
    const url = apiUrl('/api/push/test');
    if (!url) return { ok: false, reason: 'no-config' };
    try {
      const res = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ endpoint }),
      });
      const body = await res.json().catch(() => ({}));
      return {
        ok: Boolean(body.success),
        notificationSent: body.notification_sent || 0,
        // push_status/push_error: ma loi THAT tu may chu push (Apple/Google/
        // Mozilla), Worker tra ve khi gui that bai - xem cloudflare/src/index.js.
        pushStatus: body.push_status,
        pushError: body.push_error,
      };
    } catch (error) {
      return { ok: false, reason: 'error', error: String(error?.message || error) };
    }
  }

  // Trang thai tong hop de UI hien thi 1 trong 4 muc: chua ho tro / iOS chua
  // cai PWA / chua cap quyen / chua bat / da bat.
  //
  // Neu trinh duyet dang giu 1 subscription hop le (da cap quyen) nhung no
  // KHONG khop endpoint da luu cho deviceId nay (vi du sau khi trinh duyet tu
  // xoay subscription - su kien pushsubscriptionchange trong sw.js khong tu
  // goi lai server duoc vi Service Worker khong co localStorage), ham nay TU
  // dang ky lai voi Worker (upsert, an toan goi nhieu lan) thay vi bao sai
  // trang thai cho nguoi dung.
  // deviceIds: TOAN BO device_id hien co tren dashboard (khong chi may dang
  // chon) - tu dong lien ket lai may nao chua khop/chua co (them may moi,
  // subscription tu xoay...) ma khong can nguoi dung bam lai "Bat thong bao".
  async function getState(deviceIds) {
    if (!isSupported()) return { status: 'unsupported' };
    if (!cloudApiBase()) return { status: 'not-configured' };
    if (isIos() && !isStandalone()) return { status: 'ios-needs-install' };
    if (Notification.permission === 'denied') return { status: 'denied' };
    const registration = await navigator.serviceWorker.getRegistration('./sw.js');
    const subscription = await registration?.pushManager.getSubscription();
    if (!subscription || Notification.permission !== 'granted') return { status: 'not-enabled' };

    const ids = (Array.isArray(deviceIds) ? deviceIds : [deviceIds]).filter(Boolean);
    if (ids.length) {
      const linked = loadLinked();
      const missing = ids.filter((id) => linked[id]?.endpoint !== subscription.endpoint ||
        window.MayapAccount?.current?.devices.find(d=>d.device_id===id)?.linked_browsers===0);
      if (missing.length) {
        try {
          for (const id of missing) await linkSubscription(id, subscription);
        } catch (_) {
          return { status: 'error', endpoint: subscription.endpoint };
        }
      }
    }
    return { status: 'enabled', endpoint: subscription.endpoint };
  }

  // So trinh duyet dang lien ket nhan thong bao cho 1 device_id (doc tu D1
  // qua Worker, KHONG chi trinh duyet hien tai) - dung de canh bao "chua co
  // ai nhan thong bao ca". Tra ve null khi khong xac dinh duoc (mat mang,
  // device chua dang ky...) de tranh bao nham "0" khi thuc ra la chua ro.
  async function getLinkedCount(deviceId) {
    if(window.MayapAccount) {
      const device=window.MayapAccount.current?.devices.find(d=>d.device_id===deviceId);
      return typeof device?.linked_browsers==='number' ? device.linked_browsers : null;
    }
    if (!deviceId) return null;
    const url = apiUrl(`/api/device/${encodeURIComponent(deviceId)}/status`);
    if (!url) return null;
    try {
      const res = await fetch(url);
      const body = await res.json().catch(() => ({}));
      if (!body.success || !body.exists) return null;
      return typeof body.linked_browsers === 'number' ? body.linked_browsers : null;
    } catch (_) {
      return null;
    }
  }

  window.MayapPush = {
    isSupported,
    isIos,
    isStandalone,
    enable,
    disable,
    testNotification,
    getState,
    getLinkedCount,
  };
})();
