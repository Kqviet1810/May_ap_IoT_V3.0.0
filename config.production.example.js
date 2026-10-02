// Sao che file nay thanh config.js khi chuyen sang broker rieng.
// LUU Y: website GitHub Pages la ma tinh. Moi mat khau dat trong file nay
// deu co the bi xem. Ban thuong mai nen dung token ngan han do backend cap.
window.MAYAP_WEB_CONFIG = Object.freeze({
  mqttUrl: 'wss://mqtt.tenmiencuaban.vn:8084/mqtt',
  mqttUsername: '',
  mqttPassword: '',
  topicRoot: 'mayap/v1',
  reconnectPeriodMs: 2000,
  connectTimeoutMs: 15000,
  keepaliveSeconds: 30,
  sessionTtlMs: 45000,
  sessionRefreshMs: 3000,
  staleAfterMs: 8000,
  commandTimeoutMs: 10000,
  configTimeoutMs: 15000,
  // Worker Cloudflare cho kenh thong bao (thay Telegram) - xem cloudflare/README.md
  cloudApiBase: 'https://api.tenmiencuaban.vn'
});
