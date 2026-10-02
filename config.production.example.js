// Same-origin Worker + Static Assets; no broker or browser fleet credential.
window.MAYAP_WEB_CONFIG = Object.freeze({
  cloudApiBase: location.origin,
  sessionTtlMs: 45000,
  sessionRefreshMs: 15000,
  staleAfterMs: 8000,
  offlineAfterMs: 30000,
  commandTimeoutMs: 10000,
  configTimeoutMs: 15000
});
