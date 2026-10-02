const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const { webcrypto } = require('node:crypto');
const { EventEmitter } = require('node:events');
const protocol = require('../protocol_v2.js');

function browser(overrides = {}, initialStorage = {}) {
  const source = fs.readFileSync(require.resolve('../app.js'), 'utf8').replace(/  init\(\);\s*\}\)\(\);\s*$/, `
    renderDevice = () => { window.renders.push({ source: currentDevice()?.dataSource, status: connectionStatus(currentDevice()), temperature: currentDevice()?.snapshot?.runtime?.temperature }); };
    feedTelemetrySnapshot = () => {}; renderReminderList = () => {}; renderPushStatus = () => {};
    applyConfigToUi = () => {}; clearInvalid = () => {};
    invalidate = (form, id) => { window.invalidField = id; return false; };
    Object.assign(window.hooks, { state, subscribeDevice, activateSelectedSession,
      selectedNeedsSync, deactivateSession, connectMqtt, supportsVentProfile,
      swipeDestination, buildConfig, validateVentForm, REQUIRED_CONFIG_KEYS,
      VENT_PROFILE_KEYS, createDevice, connectionStatus, recoverBrowserConnection,
      refreshMqttSession, postCloudJson, isDeviceOnline, sendCommand,
      handleBootstrap, handleSnapshot, handlePresence, persistRuntimeCache, freshnessText,
      requestDeviceData, resumeBrowserConnection, controlSession, controlReady,
      prefetchControlSession, storeControlSession, controlSessions, signMqttWrite,
      enterBackground, idleBackground, warmRemainingMs, browserSessionActive,
      checkBackgroundDeadline, WARM_BACKGROUND_MS });
  })();`);
  let now = 0, wall = Date.now(), timerId = 0;
  class BrowserDate extends Date { static now() { return wall; } }
  const timers = new Map(), elements = new Map(), clients = [], events = new Map();
  const storage = new Map(Object.entries(initialStorage));
  const window = { hooks: {}, renders: [], addEventListener(name, fn) { events.set(name, fn); }, MayapProtocolV2: protocol,
    MAYAP_WEB_CONFIG: { cloudApiBase:'https://test.invalid', mqttUrl: 'wss://test.invalid/mqtt', mqttUsername: 'test', mqttPassword: 'test', sessionRefreshMs: 3000, ...overrides },
    mqtt: { connect(url, options) { const c = new EventEmitter(); c.connected = false;
      c.end = () => { c.disconnecting = true; c.emit('close'); }; c.publish = () => {};
      c.subscribe = () => {}; clients.push(c); return c; } } };
  const document = { hidden: false, body: { dataset: { page: 'device' } },
    addEventListener(name, fn) { events.set(name, fn); }, getElementById: id => elements.get(id) };
  const context = { window, document, Date: BrowserDate, crypto: webcrypto, URL, URLSearchParams, TextEncoder, AbortController,
    localStorage: { getItem: key => storage.get(key) || null, setItem: (key, value) => storage.set(key, value) }, console,
    performance: { now: () => now },
    setTimeout(fn, delay) { const id = ++timerId; timers.set(id, { fn, delay, at: wall + delay }); return id; },
    clearTimeout: id => timers.delete(id),
    setInterval(fn, delay) { const id = ++timerId; timers.set(id, { fn, delay, at: wall + delay, interval: true }); return id; },
    clearInterval: id => timers.delete(id) };
  vm.runInNewContext(source, context);
  const h = window.hooks, device = h.createDevice('MAP-1234567890AB', 'Máy thử', 'token');
  h.state.devices = [device]; h.state.selectedId = device.id;
  return { ...h, device, document, elements, window, clients, timers, context, storage, events,
    now: () => wall,
    elapse(ms, mono = ms) { wall += ms; now += mono; },
    correctClock(ms) { wall += ms; },
    tick() { for (const [id, t] of [...timers]) if (timers.has(id) && t.at <= wall) {
      if (t.interval) t.at = wall + t.delay; else timers.delete(id); t.fn();
    } },
    run(delay) { for (const [id, t] of [...timers]) if (!t.interval && t.delay === delay) {
      timers.delete(id); now += delay; wall += delay; t.fn();
    } } };
}

function connected(h) {
  const calls = [];
  h.state.mqttConnected = true;
  h.state.mqtt = { connected: true, subscribe: (filters, callback) => calls.push({ filters, callback }),
    publish: (topic, wire, options, callback) => { h.published.push({ topic, body: JSON.parse(wire), options }); callback?.(); } };
  h.published = [];
  return calls;
}

test('one batched SUBSCRIBE prioritizes four core topics and QoS, deduplicating simultaneous requests', async () => {
  const h = browser(), calls = connected(h);
  const a = h.subscribeDevice(h.device.id), b = h.subscribeDevice(h.device.id);
  assert.equal(calls.length, 1);
  const entries = Object.entries(calls[0].filters);
  assert.equal(entries.length, 4);
  assert.deepEqual(entries.map(([topic]) => topic.split('/').at(-1)), ['presence', 'bootstrap', 'snapshot', 'ack']);
  for (const [topic, options] of entries) assert.equal(options.qos, topic.endsWith('/snapshot') ? 0 : 1);
  calls[0].callback(null, entries.map(([topic, options]) => ({ topic, qos: options.qos })));
  await Promise.all([a, b]);
  await h.subscribeDevice(h.device.id);
  assert.equal(calls.length, 1);
});

test('rejected or partial SUBACK cannot authorize data synchronization', async () => {
  for (const granted of [[], [{ topic: 'wrong', qos: 0 }]]) {
    const h = browser(), calls = connected(h);
    const request = h.subscribeDevice(h.device.id);
    calls[0].callback(null, granted);
    await assert.rejects(request, /SUBSCRIBE_REJECTED/);
    assert.equal(h.state.subscriptions.size, 0);
  }
});

test('lost SUBACK times out; late or previous-connection SUBACK never populates subscriptions', async () => {
  for (const timeout of [true, false]) {
    const h = browser(), calls = connected(h);
    const request = h.subscribeDevice(h.device.id);
    if (timeout) h.run(8000); else h.state.subscriptionEpoch++;
    calls[0].callback(null, Object.entries(calls[0].filters).map(([topic, x]) => ({ topic, qos: x.qos })));
    await assert.rejects(request, timeout ? /SUBSCRIBE_TIMEOUT/ : /CONNECTION_CHANGED/);
    assert.equal(h.state.subscriptions.size, 0);
  }
});

test('lost first QoS0 sync is retried at 700ms; complete data stops extra syncs', () => {
  const h = browser(); connected(h);
  h.activateSelectedSession(true);
  assert.equal(h.published.length, 1);
  h.run(700);
  assert.equal(h.published.length, 2);
  h.device.config = Object.fromEntries(h.REQUIRED_CONFIG_KEYS.map(key => [key, 0]));
  h.device.snapshotAt = Date.now(); h.device.snapshot = { revision: 0 };
  h.device.dataSource = 'live'; h.device.liveEpoch = h.state.subscriptionEpoch;
  h.run(1600);
  assert.equal(h.published.length, 2);
});

test('sync retries stop when hidden, after device change, or on deactivation', () => {
  for (const change of [h => { h.document.hidden = true; }, h => { h.state.selectedId = 'other'; },
    h => h.deactivateSession(h.device.id)]) {
    const h = browser(); connected(h); h.activateSelectedSession(true); change(h);
    const count = h.published.length; h.run(700); h.run(1600);
    assert.equal(h.published.length, count);
  }
});

test('pairing reuses current credentials; replacing a client isolates stale callbacks', () => {
  const h = browser(); h.connectMqtt(); h.connectMqtt();
  assert.equal(h.clients.length, 1);
  const first = h.clients[0]; h.state.mqttCredentials = ['different']; h.connectMqtt();
  const second = h.clients[1]; second.connected = true; second.emit('connect');
  first.emit('close'); first.emit('offline'); first.emit('reconnect');
  assert.equal(h.state.mqttConnected, true);
  assert.equal(h.state.mqtt, second);
});

test('fan config requires complete capabilities and preserves legacy schedules', () => {
  const h = browser();
  h.device.config = Object.fromEntries(h.REQUIRED_CONFIG_KEYS.map(key => [key, 0]));
  h.device.config.ventScheduleEnabled = true; h.device.config.highTempAlarm = 39;
  for (const [id, value] of Object.entries({ ventOn: 38, ventOff: 37.8 })) h.elements.set(id, { value });
  assert.equal(h.supportsVentProfile(h.device.config), false);
  const legacy = h.buildConfig('vent');
  assert.equal(legacy.ventScheduleEnabled, true);
  assert.equal(Object.hasOwn(legacy, 'ventAutoEnabled'), false);
  for (const key of h.VENT_PROFILE_KEYS) { h.device.config[key] = 10; h.elements.set(key, { value: 10, checked: true }); }
  h.elements.get('ventProfileLevel').value = 1;
  h.elements.get('ventCycleMinutes').value = 40;
  assert.equal(h.validateVentForm(), true);
  assert.equal(h.buildConfig('vent').ventScheduleEnabled, false);
  h.elements.get('ventCycleMinutes').value = 45;
  assert.equal(h.validateVentForm(), false);
  assert.equal(h.window.invalidField, 'ventCycleMinutes');
});

test('complete cached config with stale snapshots requests sync again', () => {
  const h = browser();
  h.device.config = Object.fromEntries(h.REQUIRED_CONFIG_KEYS.map(key => [key, 0]));
  h.device.snapshot = { revision: 0 };
  h.device.snapshotAt = h.now();
  h.device.dataSource = 'live'; h.device.liveEpoch = h.state.subscriptionEpoch;
  assert.equal(h.selectedNeedsSync(), false);
  h.device.snapshotAt -= 90001;
  assert.equal(h.selectedNeedsSync(), true);
});

test('dead connected socket is replaced; broker PINGRESP and old-client packets are distinguished', async () => {
  const h = browser({ keepaliveSeconds: 30 }); h.connectMqtt();
  const first = h.clients[0]; first.connected = true; first.emit('connect');
  h.state.mqttLastPacketAt = h.now() - 90001;
  h.document.hidden = true;
  await h.recoverBrowserConnection(); assert.equal(h.clients.length, 1);
  h.document.hidden = false;
  await h.recoverBrowserConnection(); assert.equal(h.clients.length, 2);
  assert.equal(first.disconnecting, true);
  const second = h.clients[1]; second.connected = true; second.emit('connect');
  h.device.snapshotAt = h.now() - 90001; // ESP offline, broker still alive.
  h.state.mqttLastPacketAt = h.now() - 90001;
  first.emit('packetreceive', { cmd: 'pingresp' });
  assert.ok(Date.now() - h.state.mqttLastPacketAt > 90000);
  second.emit('packetreceive', { cmd: 'pingresp' });
  await h.recoverBrowserConnection(); assert.equal(h.clients.length, 2);
});

test('short telemetry staleness does not reset a healthy broker before keepalive', async () => {
  const h = browser({ staleAfterMs: 8000, offlineAfterMs: 30000, keepaliveSeconds: 30 });
  h.connectMqtt();
  const client = h.clients[0]; client.connected = true; client.emit('connect');
  for (const silence of [9000, 31000, 89000]) {
    h.device.snapshotAt = Date.now() - silence;
    h.state.mqttLastPacketAt = Date.now() - silence;
    await h.recoverBrowserConnection();
    assert.equal(h.clients.length, 1);
  }
  h.state.mqttLastPacketAt = Date.now() - 91000;
  await h.recoverBrowserConnection(); assert.equal(h.clients.length, 2);
});

test('broker silence limit never cuts below two configured keepalive intervals', async () => {
  const h = browser({ keepaliveSeconds: 60, brokerSilenceAfterMs: 8000 });
  h.connectMqtt(); h.clients[0].connected = true; h.clients[0].emit('connect');
  h.state.mqttLastPacketAt = Date.now() - 100000;
  await h.recoverBrowserConnection(); assert.equal(h.clients.length, 1);
  h.state.mqttLastPacketAt = Date.now() - 121000;
  await h.recoverBrowserConnection(); assert.equal(h.clients.length, 2);
});

test('swipe changes adjacent tabs only and rejects vertical, short, slow or edge swipes', () => {
  const h = browser();
  assert.equal(h.swipeDestination('device', -100, 5, 250), 'batch');
  assert.equal(h.swipeDestination('batch', -100, 5, 250), 'settings');
  assert.equal(h.swipeDestination('settings', 100, 5, 250), 'batch');
  for (const args of [['device', 100, 5, 250], ['settings', -100, 5, 250], ['device', -50, 1, 200],
    ['device', -100, 90, 200], ['device', -100, 1, 900]]) assert.equal(h.swipeDestination(...args), null);
});

test('pairing keeps the page and auth; production loads the pinned local MQTT client in order', () => {
  const app = fs.readFileSync(require.resolve('../app.js'), 'utf8');
  const html = fs.readFileSync(require.resolve('../index.html'), 'utf8');
  assert.doesNotMatch(app, /window\.location\.reload/);
  assert.match(app, /await verifyDevicePin\(id, pin\)/);
  assert.match(html, /defer src="\.\/vendor\/mqtt\.min\.js"/);
  assert.ok(html.indexOf('vendor/mqtt.min.js') < html.indexOf('./app.js'));
});

test('startup auth failure retries with backoff; expired pairing never retries credentials', async () => {
  for (const status of [503, 403]) {
    const h = browser(); let calls = 0;
    h.context.fetch = async () => { calls++; return { ok:false, status, json:async()=>({success:false}) }; };
    await h.refreshMqttSession();
    assert.equal(h.state.mqttSessionState, status===403 ? 'auth-required' : 'error');
    const at = h.state.authRetryAt;
    assert.ok(at > Date.now());
    await h.recoverBrowserConnection(); assert.equal(calls, 1);
    h.state.authRetryAt = 0; await h.recoverBrowserConnection();
    assert.equal(calls, status===403 ? 1 : 2);
    if (status===503) assert.equal(h.state.authRetryDelay, 20000);
    assert.equal(h.clients.length, 0);
  }
});

test('startup auth request is bounded and aborts before retry, leaving writes untouched', async () => {
  const h = browser();
  h.context.fetch = async (url, options) => new Promise((resolve,reject) => {
    options.signal.addEventListener('abort', () => reject(new Error('aborted')));
  });
  const request = h.refreshMqttSession();
  h.run(10000); await request;
  assert.equal(h.state.mqttSessionState, 'error');
  assert.equal(h.clients.length, 0);
});

test('short stale telemetry is degraded; explicit LWT and long silence still block commands', async () => {
  const h = browser({ staleAfterMs:8000, offlineAfterMs:30000 }); connected(h);
  h.device.presence = { online:true };
  h.device.snapshot = { revision:1 };
  h.device.dataSource = 'live'; h.device.liveEpoch = h.state.subscriptionEpoch;
  h.device.presenceEpoch = h.state.subscriptionEpoch;
  for (const [age, expected] of [[1000,'online'], [9000,'degraded'], [31000,'degraded']]) {
    h.device.snapshotAt = Date.now() - age;
    assert.equal(h.connectionStatus(h.device), expected);
    assert.equal(h.isDeviceOnline(h.device), age <= 30000);
  }
  h.device.snapshotAt = Date.now(); h.device.presence.online = false;
  assert.equal(h.connectionStatus(h.device), 'offline');
  h.device.presence.online = true; h.state.mqttConnected = false;
  assert.notEqual(h.connectionStatus(h.device), 'online');
  assert.equal(h.isDeviceOnline(h.device), false);
});

const runtimeKey = 'mayap.web.v10.runtime.v1.MAP-1234567890AB';
const sample = (temperature = 37.5) => ({ bootId: 123, revision: 7,
  runtime: { temperature, humidity: 58, batchRunning: true, machineState: 'DANG AP',
    lightOn: true, heaterOn: true, activeFaults: [{ code: 110, severity: 1 }] } });
const bootstrap = (publishedAt = Math.floor(Date.now() / 1000)) => ({ v: 1, proto: 2,
  bootId: 123, revision: 7, publishedAt, temperature: 37.4, humidity: 58,
  machineState: 'DANG AP', batchRunning: true, lightOn: true, faultCode: 110, faultSeverity: 1, humidifierInstalled: true });

test('cached runtime is available before MQTT and never grants live status or control', () => {
  const at = Date.now() - 60000;
  const h = browser({}, { [runtimeKey]: JSON.stringify({ v: 1, receivedAt: at,
    snapshot: sample(), presence: { online: true, proto: 2 }, presenceAt: at }) });
  assert.equal(h.clients.length, 0);
  assert.equal(h.device.snapshot.runtime.temperature, 37.5);
  assert.equal(h.device.snapshot.runtime.activeFaults[0].code, 110);
  assert.equal(h.connectionStatus(h.device), 'cache');
  assert.equal(h.isDeviceOnline(h.device), false);
  assert.equal(h.controlReady(h.device), false);
  assert.match(h.freshnessText(h.device), /Dữ liệu đã nhận/);
  connected(h);
  assert.equal(h.connectionStatus(h.device), 'cache');
  assert.equal(h.selectedNeedsSync(), true);
});

test('malformed/future cache cannot invent a live sample', () => {
  for (const wire of ['{broken', JSON.stringify({ v: 1, receivedAt: Date.now() + 120000, snapshot: sample() }),
    JSON.stringify({ v: 9, receivedAt: Date.now(), snapshot: sample() })]) {
    const h = browser({}, { [runtimeKey]: wire });
    assert.equal(h.device.snapshot, null);
    assert.equal(h.connectionStatus(h.device), 'connecting');
  }
});

test('retained bootstrap arrives during SUBSCRIBE, displays hints, then yields to a live snapshot', async () => {
  const h = browser(); h.connectMqtt();
  const c = h.clients[0];
  c.subscribe = (filters, cb) => {
    if (Object.keys(filters).some(topic => topic.endsWith('/bootstrap')))
      c.emit('message', `mayap/v1/${h.device.id}/bootstrap`, JSON.stringify(bootstrap()), { retain: true });
    cb(null, Object.entries(filters).map(([topic, x]) => ({ topic, qos: x.qos })));
  };
  c.connected = true; c.emit('connect');
  await new Promise(setImmediate);
  assert.equal(h.device.dataSource, 'bootstrap');
  assert.equal(h.connectionStatus(h.device), 'cache');
  assert.equal(h.controlReady(h.device), false);
  // Bootstrap cannot teach the control bootId or capabilities.
  assert.equal(h.device.bootId, 0);
  h.handlePresence(h.device, { online: true, bootId: 123, proto: 2 });
  h.handleSnapshot(h.device, sample(37.8));
  assert.equal(h.connectionStatus(h.device), 'online');
  assert.equal(JSON.parse(h.storage.get(runtimeKey)).features.humidifierInstalled, true);
  h.handleBootstrap(h.device, bootstrap(1));
  assert.equal(h.device.snapshot.runtime.temperature, 37.8);
  assert.equal(h.device.dataSource, 'live');
});

test('retained full snapshot and stale bootstrap cannot promote browser cache to live', () => {
  const h = browser({}, { [runtimeKey]: JSON.stringify({ v: 1, receivedAt: Date.now(), snapshot: sample() }) });
  h.connectMqtt(); const c = h.clients[0]; c.connected = true; c.emit('connect');
  c.emit('message', `mayap/v1/${h.device.id}/snapshot`, JSON.stringify(sample(99)), { retain: true });
  h.handleBootstrap(h.device, bootstrap(1));
  assert.equal(h.device.snapshot.runtime.temperature, 37.5);
  assert.equal(h.connectionStatus(h.device), 'cache');
});

test('cache writes coalesce the stream and pagehide flushes the latest sample', () => {
  const h = browser(); connected(h);
  h.handleSnapshot(h.device, sample(37.1));
  h.handleSnapshot(h.device, sample(37.9));
  assert.equal(JSON.parse(h.storage.get(runtimeKey)).snapshot.runtime.temperature, 37.1);
  h.events.get('pagehide')();
  assert.equal(JSON.parse(h.storage.get(runtimeKey)).snapshot.runtime.temperature, 37.9);
});

test('broker connected without a sample is waiting; explicit device LWT is offline', () => {
  const h = browser(); connected(h);
  assert.equal(h.connectionStatus(h.device), 'waiting');
  h.handlePresence(h.device, { online: false, bootId: 123 });
  assert.equal(h.connectionStatus(h.device), 'offline');
  h.state.mqttConnected = false;
  assert.equal(h.connectionStatus(h.device), 'connecting');
});

test('fresh presence/config cannot mask stale device telemetry or reset a healthy broker', async () => {
  const h = browser(); h.connectMqtt(); const c = h.clients[0]; c.connected = true; c.emit('connect');
  h.handlePresence(h.device, { online: true, bootId: 123 }); h.handleSnapshot(h.device, sample());
  h.device.snapshotAt -= 150000; h.device.presenceAt = Date.now(); h.device.configAt = Date.now();
  assert.equal(h.connectionStatus(h.device), 'degraded');
  await h.recoverBrowserConnection(); assert.equal(h.clients.length, 1);
});

test('lazy topics wait for the core SUBACK and request only opened data', async () => {
  const h = browser(), calls = connected(h);
  const core = h.subscribeDevice(h.device.id), lazy = h.requestDeviceData('config');
  assert.equal(calls.length, 1);
  const grant = call => call.callback(null, Object.entries(call.filters).map(([topic, x]) => ({ topic, qos: x.qos })));
  grant(calls[0]); await core; await new Promise(setImmediate);
  assert.equal(calls.length, 2);
  assert.deepEqual(Object.keys(calls[1].filters), [`mayap/v1/${h.device.id}/config/reported`]);
  grant(calls[1]); await lazy;
  assert.equal(h.published.at(-1).body.scope, 'runtime');
  assert.equal(h.published.at(-1).body.config, true);
  assert.equal(h.published.at(-1).body.reminders, false);
  assert.equal(h.published.at(-1).body.log, false);
});

test('pageshow/visibility/online storms reuse a healthy socket and restore one lease timer', async () => {
  const h = browser(); h.connectMqtt(); const c = h.clients[0];
  c.subscribe = (filters, cb) => cb(null, Object.entries(filters).map(([topic, x]) => ({ topic, qos: x.qos })));
  c.connected = true; c.emit('connect'); await new Promise(setImmediate);
  h.handlePresence(h.device, { online: true, bootId: 123 }); h.handleSnapshot(h.device, sample());
  h.document.hidden = true; h.events.get('visibilitychange')();
  assert.equal(h.state.backgroundMode, 'warm');
  assert.ok(h.state.sessionTimer);
  h.document.hidden = false;
  for (let i = 0; i < 30; i++) for (const name of ['visibilitychange', 'pageshow', 'online']) h.events.get(name)();
  await new Promise(setImmediate);
  assert.equal(h.clients.length, 1);
  assert.ok(h.state.sessionTimer);
  assert.equal(h.selectedNeedsSync(), false);
});

test('cached grant signs commands without HTTP; expired grant rejects without an HTTP hot path', async () => {
  const h = browser(); let http = 0;
  h.context.fetch = () => { http++; throw new Error('HTTP IN COMMAND'); };
  h.device.presence = { online: true, proto: 2 }; h.device.bootId = 123;
  await h.storeControlSession(h.device, { sessionKey: '07'.repeat(32), sessionId: 'test-session',
    expiresAt: Math.floor(Date.now() / 1000) + 300, grant: 'test|grant', grantSig: '08'.repeat(32) });
  const a = await h.signMqttWrite(h.device, 'command', { requestId: 'a', action: 'light_toggle' });
  const b = await h.signMqttWrite(h.device, 'command', { requestId: 'b', action: 'light_toggle' });
  const bodyA = JSON.parse(a.body), bodyB = JSON.parse(b.body);
  assert.equal(a.v, 2); assert.equal(bodyA.bootId, 123);
  assert.ok(bodyB.seq > bodyA.seq);
  assert.notEqual(bodyA.nonce, bodyB.nonce);
  assert.match(a.sig, /^[a-f0-9]{64}$/);
  h.controlSessions.get(h.device.id).expiresAt = 0;
  await assert.rejects(h.signMqttWrite(h.device, 'command', { requestId: 'c', action: 'light_toggle' }), /Đang chuẩn bị/);
  assert.equal(http, 0);
});

test('control grant preparation is single flight outside commands', async () => {
  const h = browser(); let http = 0, resolve;
  h.context.fetch = () => { http++; return new Promise(r => { resolve = r; }); };
  const a = h.refreshMqttSession(), b = h.refreshMqttSession();
  assert.equal(http, 1);
  resolve({ ok: true, status: 200, json: async () => ({ success: true,
    mqtt: { url: 'wss://test.invalid/mqtt', username: 'test', password: 'test' },
    control: { sessionKey: '07'.repeat(32), expiresAt: Math.floor(Date.now() / 1000) + 300,
      grant: 'test|grant', grantSig: '08'.repeat(32) } }) });
  assert.deepEqual(await Promise.all([a, b]), [true, true]);
  assert.equal(h.state.authRequests.size, 0);
});

test('brief hide resets resume coalescing and restores the foreground lease immediately', async () => {
  const h = browser(); h.connectMqtt(); const c = h.clients[0];
  c.subscribe = (filters, cb) => cb(null, Object.entries(filters).map(([topic, x]) => ({ topic, qos: x.qos })));
  c.connected = true; c.emit('connect'); await new Promise(setImmediate);
  h.state.lastBrowserResumeAt = Date.now();
  h.document.hidden = true; h.events.get('visibilitychange')();
  h.document.hidden = false; h.events.get('visibilitychange')();
  await new Promise(setImmediate);
  assert.ok(h.state.sessionTimer);
  assert.equal(h.clients.length, 1);
});

test('a hidden older tab cannot overwrite a newer device cache', () => {
  const h = browser(); connected(h); h.handleSnapshot(h.device, sample(37.1));
  h.storage.set(runtimeKey, JSON.stringify({ v: 1, receivedAt: Date.now() + 1, snapshot: sample(37.9) }));
  h.events.get('pagehide')();
  assert.equal(JSON.parse(h.storage.get(runtimeKey)).snapshot.runtime.temperature, 37.9);
});

test('device reboot invalidates cached config and accepts the new generation revision', () => {
  const h = browser(); connected(h);
  h.device.bootId = 122; h.device.revision = 999; h.device.config = { targetTemp: 38 };
  h.handlePresence(h.device, { online: true, bootId: 123, proto: 2 });
  assert.equal(h.device.config, null);
  h.handleSnapshot(h.device, sample());
  assert.equal(h.device.revision, 7);
  assert.equal(h.connectionStatus(h.device), 'online');
});

test('missing control grant backs off without enabling buttons or issuing repeated HTTP', async () => {
  const h = browser(); let http = 0;
  h.context.fetch = async () => { http++; return { ok:true, status:200, json:async()=>({success:true,
    mqtt:{url:'wss://test.invalid/mqtt',username:'test',password:'test'} }) }; };
  await h.refreshMqttSession();
  assert.equal(h.state.mqttSessionState, 'error');
  assert.ok(h.state.authRetryAt > Date.now());
  assert.equal(h.controlReady(h.device), false);
  h.handleSnapshot(h.device, sample()); h.prefetchControlSession();
  assert.equal(http, 1);
});

async function warmBrowser() {
  const h = browser({ sessionTtlMs:45000, connectTimeoutMs:15000, keepaliveSeconds:30,
    staleAfterMs:8000, offlineAfterMs:30000 });
  h.published = []; h.probes = []; h.connectMqtt();
  const c = h.clients[0];
  c.subscribe = (filters, options, cb) => {
    if (typeof filters === 'string') { h.probes.push({topic:filters, callback:cb}); return; }
    options(null, Object.entries(filters).map(([topic, x]) => ({topic, qos:x.qos})));
  };
  c.publish = (topic, wire, options, cb) => { if(wire) h.published.push({topic, body:JSON.parse(wire), options}); cb?.(); };
  c.connected = true; c.emit('connect'); await new Promise(setImmediate);
  h.handlePresence(h.device,{online:true,bootId:123,proto:2}); h.handleSnapshot(h.device,sample());
  await h.storeControlSession(h.device,{sessionKey:'07'.repeat(32),expiresAt:Math.floor(h.now()/1000)+600,
    grant:'warm|grant',grantSig:'08'.repeat(32)});
  h.context.fetch = () => { throw new Error('Unexpected HTTP'); };
  h.hide = () => { h.document.hidden=true; h.events.get('visibilitychange')(); };
  h.foreground = () => { h.document.hidden=false; h.events.get('visibilitychange')(); };
  return h;
}

test('30/120/179/180/299 seconds hidden stay warm and return reuses socket with immediate local signing', async () => {
  for(const seconds of [30,120,179,180,299]) {
    const h=await warmBrowser(); h.hide();
    assert.equal(h.WARM_BACKGROUND_MS,300000);
    const lease=h.published.at(-1).body;
    assert.equal(lease.active,true); assert.equal(lease.ttlMs,45000); assert.equal(lease.sync,false);
    h.elapse(seconds*1000); h.tick();
    assert.equal(h.state.backgroundMode,'warm'); assert.ok(h.browserSessionActive());
    assert.ok(h.published.at(-1).body.ttlMs <= 300000-seconds*1000);
    h.clients[0].emit('packetreceive',{cmd:'pingresp'});
    h.handleSnapshot(h.device,sample()); h.foreground(); await new Promise(setImmediate);
    assert.equal(h.clients.length,1); assert.equal(h.probes.length,0);
    assert.equal(h.state.backgroundMode,'visible'); assert.ok(h.controlReady(h.device));
    assert.equal((await h.signMqttWrite(h.device,'command',{requestId:'return',action:'light_toggle'})).v,2);
  }
});

test('300 seconds hidden becomes idle, stops lease/grant refresh, retains socket and reuses it after long hide', async () => {
  const h=await warmBrowser(); h.hide(); h.elapse(300000); h.tick();
  assert.equal(h.state.backgroundMode,'idle'); assert.equal(h.state.sessionTimer,0);
  assert.equal(h.published.at(-1).body.active,false); assert.equal(h.clients.length,1);
  const count=h.published.length; h.elapse(600000); h.tick(); h.prefetchControlSession();
  assert.equal(h.published.length,count); assert.equal(h.clients[0].disconnecting,undefined);
  // Restore a still-valid grant; no HTTP may be required on the healthy return.
  h.controlSessions.get(h.device.id).expiresAt=Math.floor(h.now()/1000)+300;
  h.clients[0].emit('packetreceive',{cmd:'pingresp'}); h.handleSnapshot(h.device,sample());
  h.foreground(); await new Promise(setImmediate);
  assert.equal(h.clients.length,1); assert.equal(h.published.at(-1).body.active,true);
  assert.ok(h.controlReady(h.device));
});

test('OS-frozen timers use wall timestamp on resume and stale runtime requests sync without a new WSS', async () => {
  const h=await warmBrowser(); h.hide(); h.elapse(360000,0);
  assert.equal(h.state.backgroundMode,'warm'); // No timer was delivered by the OS.
  h.foreground(); await new Promise(setImmediate);
  assert.equal(h.state.backgroundMode,'visible'); assert.equal(h.clients.length,1);
  assert.ok(h.published.some(x=>x.body.active===false));
  assert.equal(h.published.at(-1).body.sync,true);
  assert.equal(h.probes.length,1);
  h.probes[0].callback(null,[]); h.tick();
  assert.equal(h.clients.length,1); assert.equal(h.state.brokerProbe,null);
});

test('silent suspended socket probes once; a broker response reuses it even with device offline', async () => {
  const h=await warmBrowser(); h.hide(); h.elapse(400000,0); h.foreground();
  for(let i=0;i<30;i++) for(const name of ['pageshow','online','visibilitychange']) h.events.get(name)();
  await new Promise(setImmediate);
  assert.equal(h.probes.length,1); assert.equal(h.clients.length,1);
  h.handlePresence(h.device,{online:false,bootId:123});
  h.clients[0].emit('packetreceive',{cmd:'pingresp'}); h.elapse(15000); h.tick();
  assert.equal(h.clients.length,1); assert.equal(h.connectionStatus(h.device),'offline');
});

test('dead background socket gets exactly one replacement after one bounded broker probe', async () => {
  const h=await warmBrowser(); h.hide(); h.elapse(400000,0); h.foreground();
  await new Promise(setImmediate);
  assert.equal(h.clients.length,1); assert.equal(h.probes.length,1);
  h.elapse(15000); h.tick(); await new Promise(setImmediate);
  for(let i=0;i<30;i++) h.resumeBrowserConnection({type:'online'});
  assert.equal(h.clients.length,2); assert.equal(h.clients[0].disconnecting,true);
  // A late old SUBACK cannot change the new client's liveness.
  const at=h.state.mqttLastPacketAt; h.elapse(1000); h.probes[0].callback(null,[]);
  assert.equal(h.state.mqttLastPacketAt,at);
});

test('MQTT.js already retrying a closed socket stays the single reconnect owner on resume', async () => {
  const h=await warmBrowser(); h.hide(); h.clients[0].connected=false; h.clients[0].emit('close');
  h.elapse(400000); h.foreground();
  for(let i=0;i<30;i++) h.resumeBrowserConnection({type:'online'});
  assert.equal(h.clients.length,1); assert.equal(h.probes.length,0);
  h.clients[0].connected=true; h.clients[0].emit('connect'); await new Promise(setImmediate);
  assert.equal(h.published.at(-1).body.active,true); assert.equal(h.clients.length,1);
});

test('traffic early in WARM followed by OS suspension still probes before replacing the resumed socket', async () => {
  const h=await warmBrowser(); h.hide(); h.elapse(30000); h.tick();
  h.clients[0].emit('packetreceive',{cmd:'pingresp'});
  assert.equal(h.state.mqttResumeProbeRequired,false);
  h.elapse(360000,0); h.foreground(); await new Promise(setImmediate);
  assert.equal(h.clients.length,1); assert.equal(h.probes.length,1);
  h.probes[0].callback(null,[]); h.elapse(15000); h.tick();
  assert.equal(h.clients.length,1);
});

test('warm grant renewal is proactive/single-flight and stops in idle; expired clicks never do HTTP', async () => {
  const h=await warmBrowser(); let http=0, finish;
  h.context.fetch=()=>{http++; return new Promise(resolve=>{finish=resolve;});};
  h.controlSessions.get(h.device.id).expiresAt=Math.floor(h.now()/1000)+300;
  h.hide(); h.elapse(239000); h.tick(); h.prefetchControlSession(); assert.equal(http,0);
  h.elapse(2000); h.tick(); for(let i=0;i<20;i++) h.prefetchControlSession();
  assert.equal(http,1);
  finish({ok:true,status:200,json:async()=>({success:true,mqtt:{url:'wss://test.invalid/mqtt',username:'test',password:'test'},
    control:{sessionKey:'07'.repeat(32),expiresAt:Math.floor(h.now()/1000)+300,grant:'renew|grant',grantSig:'08'.repeat(32)}})});
  await new Promise(setImmediate);
  h.elapse(60000); h.tick(); assert.equal(h.state.backgroundMode,'idle');
  h.controlSessions.get(h.device.id).expiresAt=0;
  h.prefetchControlSession(); assert.equal(http,1);
  await assert.rejects(h.signMqttWrite(h.device,'command',{requestId:'expired',action:'light_toggle'}),/Đang chuẩn bị/);
  assert.equal(http,1); h.foreground(); await new Promise(setImmediate);
  assert.equal(http,2,'Resume prepares the grant outside command handling');
  finish({ok:false,status:503,json:async()=>({})}); await new Promise(setImmediate);
});

test('warm deadline survives 32-bit timestamp boundary, backwards clock and duplicate hidden/BFCache events', async () => {
  const h=await warmBrowser(); h.correctClock(0xffffffff-1000-h.now()); h.hide();
  h.elapse(120000); h.events.get('pagehide')({persisted:true}); h.events.get('visibilitychange')();
  assert.equal(h.warmRemainingMs(),180000); // Duplicate events cannot restart five minutes.
  h.correctClock(-3600000); h.elapse(179999); assert.equal(h.warmRemainingMs(),1);
  h.elapse(1); h.checkBackgroundDeadline(); assert.equal(h.state.backgroundMode,'idle');
  assert.equal(h.clients.length,1);
});

test('late probe timeout after another suspension rechecks broker instead of declaring a healthy socket dead', async () => {
  const h=await warmBrowser(); h.hide(); h.elapse(400000,0); h.foreground();
  await new Promise(setImmediate); h.elapse(120000,0); h.tick();
  assert.equal(h.clients.length,1); assert.equal(h.probes.length,2);
  h.probes[1].callback(null,[]); assert.equal(h.state.brokerProbe,null);
});
