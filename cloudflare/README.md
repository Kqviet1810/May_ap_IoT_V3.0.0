# MAYAP Cloudflare — proposed V1.1.0

Workers Static Assets hosts the existing PWA. `src/account-worker.js` keeps
Google accounts, ownership, provisioning, API, D1 and Push, and admits authenticated
WebSockets to one GA SQLite `DeviceHub` per machine. DeviceHub routes signed V2
messages and ESP32 ACKs using hibernating sockets; it does not control the machine.

The full contract, Free-plan worksheet, cutover, manual settings and hardware
checklist are in [the migration guide](../doc/CLOUDFLARE_REALTIME_MIGRATION.md).
The old MQTT runtime and fleet credentials have been removed.

## Local setup

```bash
npx --yes pnpm@11.19.0 install --frozen-lockfile --ignore-scripts
python3 ../tools/build_web_assets.py
npx pnpm@11.19.0 dev
```

Real Google login requires the configured authorized HTTPS origin. Automated tests
seed an isolated **local** D1 fixture without weakening production login:

```bash
cd ..
node --test tests/*.test.cjs
python3 tools/test_realtime_workerd.py
# Also test native Chromium (install pinned Playwright Chromium first):
python3 tools/test_realtime_workerd.py --browser
```

The asset builder uses an explicit public-file allowlist. Firmware, tests, node
modules, private keys, config overrides and `.dev.vars` cannot become public assets.

## Resources and settings

- `DB`: existing D1 `mayap_push`, migration history `0001`–`0005`.
- `DEVICE_HUB`: DeviceHub SQLite DO, migration `device-hub-v1`.
- `ASSETS`: generated `public/`; APIs and /realtime/* run the Worker first.
- Existing one-minute Cron for sparse offline/Push state remains.
- `ALLOWED_ORIGIN`: exact final HTTPS Worker/custom-domain origin.
- `GOOGLE_CLIENT_ID`: existing GIS web client; authorize the final origin in Google.
- Provisioning defaults and factory inventory enable/disable checks remain.
- Runtime pinned to compatibility date 2026-10-01 and nodejs_compat.

Preserve existing `DEVICE_KEY_PEPPER` and `MAYAP_SESSION_PEPPER`. Push requires
VAPID_PUBLIC_KEY, VAPID_PRIVATE_KEY, VAPID_SUBJECT. GITHUB_TOKEN remains optional
for release reads. There are no broker/fleet WebSocket credentials. Existing
unique device keys are in NVS and peppered D1 hashes. Secrets stay in Cloudflare
Dashboard/`wrangler secret put`, never source, static config or URLs.

## Deployment

The reviewed GitHub Actions deployment runs local checks/workerd, stages assets,
applies D1 migrations, then deploys Worker/assets/DO migration together. It can be
run manually on a selected ref. After commissioning, repository variable
`CLOUDFLARE_REALTIME_AUTODEPLOY=1` enables deployment of the exact successful main
build SHA. GitHub secrets are CLOUDFLARE_API_TOKEN and CLOUDFLARE_ACCOUNT_ID.

No remote deployment or OTA release was performed for this PR. First provision an
isolated pilot Worker/D1 and test one wired bench ESP32. Coordinate cloud/Web
cutover with device commissioning: existing MQTT firmware cannot speak WebSocket.
Use GitHub Actions as the deployment source; do not enable a second automatic
Cloudflare Builds pipeline for the same production Worker.
