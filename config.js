// Same-origin Cloudflare Static Assets + account/API + native realtime.
(() => {
  'use strict';
  try {
    localStorage.removeItem('mayap.web.v10.mqtt.private');
    localStorage.removeItem('mayap.web.v10.devices');
  } catch (_) {}
  window.MAYAP_WEB_CONFIG = Object.freeze({
    cloudApiBase: location.origin,
    connectTimeoutMs: 15000, keepaliveSeconds: 30,
    sessionTtlMs: 45000, sessionRefreshMs: 15000,
    staleAfterMs: 8000, offlineAfterMs: 30000,
    commandTimeoutMs: 10000, configTimeoutMs: 15000,
  });
})();
