'use strict';
const CACHE = 'mayap-web-v12.2.2';
const APP_SHELL = [
  './', './index.html', './styles.css', './landing.css', './account.js', './config.js', './app.js', './protocol_v2.js', './push.js', './manifest.webmanifest',
  './vendor/jsQR.min.js', './vendor/mqtt.min.js',
  './docs/MAYAP_Huong_dan_van_hanh_A5_v1.3_E503.pdf',
  './icons/icon-192.png', './icons/icon-512.png', './icons/badge-72.png'
];
self.addEventListener('install', (event) => {
  // Dung tung cache.add() + catch rieng thay vi cache.addAll() (all-or-nothing):
  // 1 file loi (404/mang cham luc cai dat) tung lam TOAN BO Service Worker
  // khong cai dat duoc, chan luon ca tinh nang Push (phu thuoc SW). Thieu 1
  // file trong app shell chi lam file do khong duoc cache truoc, khong chan
  // ca trang.
  event.waitUntil(
    caches.open(CACHE).then((cache) => Promise.all(
      APP_SHELL.map((url) => cache.add(url).catch(() => {}))
    ))
  );
  self.skipWaiting();
});
self.addEventListener('activate', (event) => {
  event.waitUntil(caches.keys().then((keys) => Promise.all(
    keys.filter((key) => key !== CACHE).map((key) => caches.delete(key))
  )));
  self.clients.claim();
});
// A warm app shell must not wait indefinitely for a weak/mobile connection.
// Prefer fresh code, then fall back to THIS release's cache after 250ms, keeping
// the refresh alive. Only public same-origin static assets use this path.
function networkFirstCore(event) {
  const cacheReady = caches.open(CACHE).catch(() => null);
  const cachedReady = cacheReady.then(cache => cache?.match(event.request)).catch(() => undefined);
  const refresh = fetch(event.request).then(async response => {
    if (!response.ok) return (await cachedReady) || response;
    try { await (await cacheReady)?.put(event.request, response.clone()); } catch (_) {}
    return response;
  }).catch(async () => (await cachedReady) || Response.error());
  // Register the extended lifetime synchronously inside the fetch event.
  event.waitUntil(refresh.then(() => {}));
  return cachedReady.then(async cached => {
    if (!cached) return refresh;
    let timer;
    try {
      return await Promise.race([refresh, new Promise(resolve => {
        timer = setTimeout(() => resolve(cached), 250);
      })]);
    } finally { clearTimeout(timer); }
  });
}

self.addEventListener('fetch', (event) => {
  if (event.request.method !== 'GET') return;
  const url = new URL(event.request.url);
  if (url.origin !== self.location.origin || url.pathname.startsWith('/api/') || url.pathname.startsWith('/auth/')) return;
  // Pinned bundles use this release's cache; app code prefers fresh responses.
  if (/\/vendor\/(?:mqtt|jsQR)\.min\.js$/.test(url.pathname)) {
    event.respondWith(caches.open(CACHE).then(async (cache) => {
      const cached = await cache.match(event.request);
      if (cached) return cached;
      const response = await fetch(event.request);
      if (response.ok) await cache.put(event.request, response.clone());
      return response;
    }));
    return;
  }
  // Background refresh preserves updates even when a cached shell wins.
  const isCoreAsset = /\.(?:html|js|css)$/.test(url.pathname) || url.pathname.endsWith('/');
  if (isCoreAsset) {
    event.respondWith(networkFirstCore(event));
    return;
  }
  event.respondWith(caches.match(event.request).then((cached) => cached || fetch(event.request).then((response) => {
    const copy = response.clone();
    caches.open(CACHE).then((cache) => cache.put(event.request, copy));
    return response;
  })));
});

// ------------------------------- Web Push -----------------------------------
// Nhan push tu Cloudflare Worker (thay Telegram) va hien notification that su
// cua he dieu hanh - hoat dong ke ca khi khong co tab nao cua website dang mo.
self.addEventListener('push', (event) => {
  let data = {};
  try { data = event.data ? event.data.json() : {}; } catch (_) {
    data = { title: 'MAYAP', body: event.data ? event.data.text() : '' };
  }

  const title = data.title || 'MAYAP';
  const options = {
    body: data.body || '',
    icon: data.icon || './icons/icon-192.png',
    badge: data.badge || './icons/badge-72.png',
    data: data.data || {},
    tag: data.data?.alarmType ? `mayap-${data.data.deviceId || ''}-${data.data.alarmType}` : undefined,
    // Canh bao ACTIVE thay the ban cu cung loai (khong xep chong nhieu thong
    // bao "van con loi X" giong nhau); tin RESOLVED luon la thong bao rieng
    // (khong ghi de) de nguoi dung con thay ro da tung co canh bao.
    renotify: data.data?.state === 'active',
  };

  event.waitUntil(self.registration.showNotification(title, options));
});

self.addEventListener('notificationclick', (event) => {
  event.notification.close();
  const targetUrl = new URL(event.notification.data?.url || './', self.location.href).href;
  event.waitUntil(
    self.clients.matchAll({ type: 'window', includeUncontrolled: true }).then((clients) => {
      for (const client of clients) {
        if (client.url === targetUrl && 'focus' in client) return client.focus();
      }
      if (self.clients.openWindow) return self.clients.openWindow(targetUrl);
      return undefined;
    })
  );
});

self.addEventListener('pushsubscriptionchange', (event) => {
  // Trinh duyet tu xoay subscription (het han/thu hoi khoa) - trang web se tu
  // phat hien va dang ky lai o lan mo tiep theo qua MayapPush.getState() trong
  // push.js, khong can xu ly gi them o day.
});
