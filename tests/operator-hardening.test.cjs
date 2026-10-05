const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const read = (name) => fs.readFileSync(path.resolve(__dirname, '..', name), 'utf8');
const dir = 'MAYAP_INDUSTRIAL_v1_0_0/';
const config = read(dir + 'config.h');
const ino = read(dir + 'MAYAP_INDUSTRIAL_v1_0_0.ino');
const network = read(dir + 'network_service.h');
const machine = read(dir + 'machine_control.h');
const hmi = read(dir + 'hmi.h');
const html = read('index.html');
const app = read('app.js');
const vm = require('node:vm');

test('boot home release is sensor-resolved and independent of Wi-Fi', () => {
  assert.match(machine, /bool startupResolved\(\) const \{ return startupResolved_; \}/);
  assert.match(machine, /bool sensorStartupResolved\(\) const \{ return sensor_\.startupResolved\(\); \}/);
  assert.match(ino, /sensorStartupResolved/);
  assert.match(ino, /sensorResolved[\s\S]{0,300}localTaskStability\.held\(now, homeDelay\)/);
  assert.doesNotMatch(hmi, /mayapBootHomeReleased\(\) \|\|/);
});

test('E405 is a local non-sounding reminder with periodic HMI presentation', () => {
  assert.match(machine, /WifiDisconnected = 405/);
  assert.match(machine, /FaultCode::WifiDisconnected, FaultSeverity::Info, 20U, AlarmNone, false, false, false, false, false, false/);
  assert.match(machine, /status\.requestedMode == ConnectivityMode::Online[\s\S]{0,300}!status\.connected[\s\S]{0,350}WifiDisconnected/);
  assert.match(hmi, /case 405: return AlarmNone/);
  assert.match(hmi, /case 405: return "MAT KET NOI WIFI"/);
  assert.match(hmi, /600000UL/);
  assert.match(hmi, /showToast\("E405 MAT WIFI"/);
  assert.doesNotMatch(machine, /FaultCode::WifiDisconnected[^\n]*AlarmSystem/);
});

test('connection info uses owner-published SSID and signal line for loss', () => {
  assert.match(config, /char ssid\[33\]/);
  assert.match(config, /char networkSsid\[33\]/);
  assert.match(network, /publishedSsid/);
  assert.match(network, /snprintf\(status\.ssid, sizeof\(status\.ssid\), "%s", publishedSsid\)/);
  assert.match(machine, /runtime_\.networkSsid/);
  assert.match(hmi, /"WIFI: %s", currentRuntime\.networkSsid/);
  assert.match(hmi, /"SONG: MAT KET NOI"/);
  assert.match(hmi, /"SONG: DANG KET NOI"/);
  assert.doesNotMatch(hmi.replace(/\/\/[^\n]*/g, ''), /WiFi\.SSID\(/);
  assert.doesNotMatch(hmi, /WIFI: DA KET NOI/);
});

test('actual E405 predicate excludes offline, unconfigured, active portal and stable connected', () => {
  const expression = machine.match(/const bool wifiOfflineCandidate =([\s\S]*?);/)[1].replace(/::/g, '.');
  const ConnectivityMode = { Online: 1, Offline: 0 };
  const NetworkStateCode = { Offline: 0, NotConfigured: 1, Connecting: 2, Connected: 3 };
  const WifiPortalState = { Idle: 0, Failed: 4, Active: 1 };
  const evaluate = (overrides = {}, portalState = WifiPortalState.Idle) => vm.runInNewContext(expression, {
    status: { requestedMode: 1, credentialsConfigured: true, state: 2, connected: false, ...overrides },
    portal: { state: portalState }, ConnectivityMode, NetworkStateCode, WifiPortalState,
  });
  assert.equal(evaluate(), true);
  assert.equal(evaluate({}, WifiPortalState.Failed), true);
  assert.equal(evaluate({ requestedMode: 0 }), false);
  assert.equal(evaluate({ credentialsConfigured: false }), false);
  assert.equal(evaluate({}, WifiPortalState.Active), false);
  assert.equal(evaluate({ state: 0 }), false);
  assert.equal(evaluate({ state: 1 }), false);
  assert.equal(evaluate({ connected: true, state: 3 }), false);
});

test('bitless reminder screen remains inspectable and can be closed without a sounding ACK', () => {
  assert.match(hmi, /!runtime\.alarmMask && !runtime\.activeFaultDisplayCount && view == View::Alarm/);
  assert.match(hmi, /if \(!snapshot\) \{\s*if \(currentRuntime\.activeFaultDisplayCount\) \{[\s\S]*?return true;/);
});

test('actual splash release condition never bypasses unresolved startup', () => {
  const expression = ino.match(/if \((!bootReadyShown && sensorResolved &&[\s\S]*?homeDelay\))\) \{/)[1];
  for (const sensorResolved of [false, true]) for (const held of [false, true]) {
    assert.equal(vm.runInNewContext(expression, { bootReadyShown: false, sensorResolved,
      localTaskStability: { held: () => held }, now: 10000, homeDelay: 1500 }), sensorResolved && held);
  }
});

test('actual HMI reminder repeats at ten minutes and resets on reconnect without output actions', () => {
  const start = hmi.indexOf('  const bool onlineConfigured =');
  const code = hmi.slice(start, hmi.indexOf('  const bool weak =', start))
    .replace(/\bconst bool\b/g, 'const').replace(/::/g, '.').replace(/(\d+)(?:UL|U)\b/g, '$1');
  const context = { now: 100, ConnectivityMode: { Online: 1 }, WifiPortalState: { Idle: 0, Failed: 4 },
    currentRuntime: { connectivityMode: 1, networkConfigured: true, networkConnected: false, wifiPortalState: 0 },
    wifiOfflineTracking: false, wifiOfflineNoticeActive: false, wifiOfflineSinceAt: 0,
    wifiOfflineLastReminderAt: 0, wifiStableLossObserved: false, reminders: [], showToast: (...args) => context.reminders.push(args[0]) };
  vm.createContext(context);
  for (const now of [100, 5099, 5100, 605099, 605100]) { context.now = now; vm.runInContext(`{${code}}`, context); }
  assert.deepEqual(context.reminders, ['E405 MAT WIFI', 'E405 MAT WIFI']);
  context.currentRuntime.networkConnected = true;
  vm.runInContext(`{${code}}`, context);
  assert.equal(context.wifiOfflineNoticeActive, false);
  assert.equal(context.wifiStableLossObserved, false);
  assert.doesNotMatch(code, /buzzer|siren|SSR|restart|heater|turning/);
});

test('actual thermal UI tolerates missing fields and renders all runtime states', () => {
  const start = app.indexOf('    const adaptiveNames =');
  const code = app.slice(start, app.indexOf('    const autoTuneState', start));
  for (const [adaptiveThermal, expected] of [[undefined, 'Cân bằng nhiệt thông minh'], [null, 'Cân bằng nhiệt thông minh'], [{}, 'Cân bằng nhiệt thông minh'],
    [{ state: 0 }, 'Cân bằng nhiệt · Tắt'], [{ state: 1 }, 'Cân bằng nhiệt · Đang học'], [{ state: 3 }, 'Cân bằng nhiệt · Đang cân bằng'],
    [{ state: 4 }, 'Cân bằng nhiệt · Tự làm mát'], [{ state: 6 }, 'Cân bằng nhiệt · Tạm ngưng']]) {
    const nodes = { thermalBalanceFeature: {}, adaptiveThermalStatus: {} };
    vm.runInNewContext(code, { runtime: { adaptiveThermal }, $: id => nodes[id] });
    assert.equal(nodes.thermalBalanceFeature.textContent, expected);
  }
  const entry = app.slice(app.indexOf('  function applySnapshotToUi(device)'), start);
  assert.ok(entry.indexOf("textContent = 'Cân bằng nhiệt thông minh'") < entry.indexOf('if (!runtime)'));
});

test('logged-in device page introduces adaptive thermal balance without changing its control path', () => {
  assert.match(html, /id="page-device"[\s\S]*id="thermalBalanceFeature"/);
  assert.match(html, /Cân bằng nhiệt thông minh/);
  assert.match(app, /thermalBalanceFeature/);
  assert.match(app, /adaptiveNames/);
  assert.match(app, /bảo vệ nhiệt vẫn độc lập/);
});
