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
    runtimeRealtime = { deviceId:'MAP-1234567890AB', url:'wss://test.invalid/realtime/browser/MAP-1234567890AB', ticket:'test-ticket' };
    Object.assign(window.hooks, { state, subscribeDevice, activateSelectedSession,
      selectedNeedsSync, deactivateSession, connectRealtime, supportsVentProfile,
      swipeDestination, buildConfig, validateVentForm, validateAdvancedForm, REQUIRED_CONFIG_KEYS,
      VENT_PROFILE_KEYS, createDevice, connectionStatus, recoverBrowserConnection,
      refreshRealtimeSession, requestRealtimeSession, postCloudJson, isDeviceOnline, sendCommand,
      handleBootstrap, handleSnapshot, handlePresence, persistRuntimeCache, freshnessText,
      requestDeviceData, resumeBrowserConnection, controlSession, controlReady,
      prefetchControlSession, storeControlSession, controlSessions, signRealtimeWrite,
      enterBackground, idleBackground, warmRemainingMs, browserSessionActive,
      checkBackgroundDeadline, WARM_BACKGROUND_MS });
  })();`);
  let now = 0, wall = Date.now(), timerId = 0;
  class BrowserDate extends Date { static now() { return wall; } }
  const timers = new Map(), elements = new Map(), clients = [], events = new Map();
  const storage = new Map(Object.entries(initialStorage));
  const window = { hooks: {}, renders: [], addEventListener(name, fn) { events.set(name, fn); }, MayapProtocolV2: protocol,
    MAYAP_WEB_CONFIG: { cloudApiBase:'https://test.invalid', realtimeUrl: 'wss://test.invalid/realtime', realtimeUsername: 'test', realtimePassword: 'test', sessionRefreshMs: 3000, ...overrides },
    MayapRealtime: { Client: class extends EventEmitter {
      constructor(options) { super(); this.deviceId=options.deviceId; this.connected=false; this.resumes=0; clients.push(this); }
      end() { this.disconnecting=true; this.emit('close'); }
      send() {} renew() {} probe() { this.resumes++; } resume() { this.resumes++; }
    } } };

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
  h.state.realtimeConnected = true; h.published = [];
  h.state.realtime = { deviceId:h.device.id, connected:true, send:(topic, body, callback)=>{h.published.push({topic,body});callback?.();}, resume(){}, renew(){} };
}

test('advanced UI hides SSR cycle while preserving legacy protocol readback', () => {
  const html = fs.readFileSync(require.resolve('../index.html'), 'utf8');
  assert.doesNotMatch(html, /advPidCycleSec/);
  for (const pidCycleSec of [1, 10, 60]) {
    const h = browser();
    h.device.config = Object.fromEntries(h.REQUIRED_CONFIG_KEYS.map(key => [key, 0]));
    h.device.config.pidCycleSec = pidCycleSec;
    const values = { advKp:18, advKi:0.8, advKd:45, advMaxHeaterPower:100,
      advTempRateLimitC:1, advTempRateWindowSec:120, advTempOscillationCrossLimit:6,
      advTempOscillationWindowSec:600, advHeaterStuckMinRiseC:0.3,
      advHeaterStuckDurationSec:900, advAutotuneRelayPowerPercent:30, advAutotuneBandC:0.2 };
    for (const [id,value] of Object.entries(values)) h.elements.set(id,{value});
    assert.equal(h.validateAdvancedForm(), true);
    assert.equal(h.buildConfig('advanced').pidCycleSec, pidCycleSec);
  }
});

test('adaptive thermal setting is explicit opt-in and omitted for legacy firmware', () => {
  const html=fs.readFileSync(require.resolve('../index.html'),'utf8');
  assert.match(html,/Tự cân bằng nhiệt/);
  assert.match(html,/id="adaptiveThermalBalanceEnabled" type="checkbox" disabled/);
  const h=browser();h.device.config=Object.fromEntries(h.REQUIRED_CONFIG_KEYS.map(key=>[key,0]));
  const values={advKp:18,advKi:.8,advKd:45,advMaxHeaterPower:100,advTempRateLimitC:1,
    advTempRateWindowSec:120,advTempOscillationCrossLimit:6,advTempOscillationWindowSec:600,
    advHeaterStuckMinRiseC:.3,advHeaterStuckDurationSec:900,advAutotuneRelayPowerPercent:30,advAutotuneBandC:.2};
  for(const [id,value] of Object.entries(values))h.elements.set(id,{value});
  h.elements.set('adaptiveThermalBalanceEnabled',{checked:true});
  assert.equal(Object.hasOwn(h.buildConfig('advanced'),'adaptiveThermalBalanceEnabled'),false);
  h.device.config.adaptiveThermalBalanceEnabled=false;
  assert.equal(h.buildConfig('advanced').adaptiveThermalBalanceEnabled,true);
  h.elements.get('adaptiveThermalBalanceEnabled').checked=false;
  const off=h.buildConfig('advanced');assert.equal(off.adaptiveThermalBalanceEnabled,false);
  assert.equal(off.kp,18);assert.equal(off.ki,.8);assert.equal(off.kd,45);
});

test('admitted selected socket is reused without broker subscriptions', async () => {
  const h=browser(); connected(h);
  await Promise.all([h.subscribeDevice(h.device.id),h.subscribeDevice(h.device.id)]);
  assert.equal(h.published.length,0);
});
test('another device cannot initiate synchronization through selected socket', async () => {
  const h=browser(); connected(h); await h.subscribeDevice('MAP-000000000000');
  assert.equal(h.published.length,0);
});
test('absent admission cannot authorize writes', async () => {
  const h=browser(); h.device.snapshot={bootId:123}; h.device.bootId=123;
  await h.sendCommand('light_toggle'); assert.equal(h.clients.length,0);
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
  const h = browser(); h.connectRealtime(); h.connectRealtime();
  assert.equal(h.clients.length, 1);
  const first = h.clients[0]; h.connectRealtime(true);
  const second = h.clients[1]; second.connected = true; second.emit('connect');
  first.emit('close'); first.emit('offline'); first.emit('reconnect');
  assert.equal(h.state.realtimeConnected, true);
  assert.equal(h.state.realtime, second);
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

test('resume delegates liveness to the transport and fences old clients', async () => {
  const h=browser(); h.connectRealtime(); const first=h.clients[0]; first.connected=true; first.emit('connect');
  h.document.hidden=true; await h.recoverBrowserConnection(); assert.equal(first.resumes,0);
  h.document.hidden=false; await h.recoverBrowserConnection(); assert.equal(first.resumes,1);
  h.connectRealtime(true); const second=h.clients[1]; second.connected=true; second.emit('connect');
  const at=h.state.realtimeLastPacketAt; first.emit('packetreceive'); assert.equal(h.state.realtimeLastPacketAt,at);
  assert.equal(h.state.realtime,second);
});
test('stale device telemetry never replaces a healthy Cloudflare socket', async () => {
  const h=browser(); h.connectRealtime(); const c=h.clients[0]; c.connected=true; c.emit('connect');
  for(const silence of [9000,31000,89000,121000]) {h.device.snapshotAt=h.now()-silence; await h.recoverBrowserConnection();}
  assert.equal(h.clients.length,1); assert.equal(c.resumes,4);
});
test('liveness policy resides in bounded native transport', () => {
  const transport=fs.readFileSync(require.resolve('../realtime_transport.js'),'utf8');
  assert.match(transport,/STALE_MS = 90000/); assert.match(transport,/}, 8000\)/);
});

test('swipe changes adjacent tabs only and rejects vertical, short, slow or edge swipes', () => {
  const h = browser();
  assert.equal(h.swipeDestination('device', -100, 5, 250), 'batch');
  assert.equal(h.swipeDestination('batch', -100, 5, 250), 'settings');
  assert.equal(h.swipeDestination('settings', 100, 5, 250), 'batch');
  for (const args of [['device', 100, 5, 250], ['settings', -100, 5, 250], ['device', -50, 1, 200],
    ['device', -100, 90, 200], ['device', -100, 1, 900]]) assert.equal(h.swipeDestination(...args), null);
});

test('pairing keeps the page and auth; production loads the native WebSocket client in order', () => {
  const app = fs.readFileSync(require.resolve('../app.js'), 'utf8');
  const html = fs.readFileSync(require.resolve('../index.html'), 'utf8');
  assert.doesNotMatch(app, /window\.location\.reload/);
  assert.match(app, /await verifyDevicePin\(id, pin\)/);
  assert.match(html, /defer src="\.\/realtime_transport\.js"/);
  assert.ok(html.indexOf('realtime_transport.js') < html.indexOf('./app.js'));
});

test('startup auth failure retries with backoff; expired pairing never retries credentials', async () => {
  for (const status of [503, 403]) {
    const h = browser(); let calls = 0;
    h.context.fetch = async () => { calls++; return { ok:false, status, json:async()=>({success:false}) }; };
    await h.refreshRealtimeSession();
    assert.equal(h.state.realtimeSessionState, status===403 ? 'auth-required' : 'error');
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
  const request = h.refreshRealtimeSession();
  h.run(10000); await request;
  assert.equal(h.state.realtimeSessionState, 'error');
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
  h.device.presence.online = true; h.state.realtimeConnected = false;
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

test('cached runtime is available before FRAME and never grants live status or control', () => {
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
  const h = browser(); h.connectRealtime();
  const c = h.clients[0];
  c.connected = true; c.emit('connect');
  c.emit('message', {deviceId:h.device.id,channel:'bootstrap'}, bootstrap(), {cached:true});
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
  h.connectRealtime(); const c = h.clients[0]; c.connected = true; c.emit('connect');
  c.emit('message', {deviceId:h.device.id,channel:'snapshot'}, sample(99), {cached:true});
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
  h.state.realtimeConnected = false;
  assert.equal(h.connectionStatus(h.device), 'connecting');
});

test('fresh presence/config cannot mask stale device telemetry or reset a healthy broker', async () => {
  const h = browser(); h.connectRealtime(); const c = h.clients[0]; c.connected = true; c.emit('connect');
  h.handlePresence(h.device, { online: true, bootId: 123 }); h.handleSnapshot(h.device, sample());
  h.device.snapshotAt -= 150000; h.device.presenceAt = Date.now(); h.device.configAt = Date.now();
  assert.equal(h.connectionStatus(h.device), 'degraded');
  await h.recoverBrowserConnection(); assert.equal(h.clients.length, 1);
});

test('on-demand synchronization requests only opened data on the admitted socket', async () => {
  const h=browser(); connected(h); await h.requestDeviceData('config');
  const body=h.published.at(-1).body;
  assert.equal(body.scope,'runtime'); assert.equal(body.config,true); assert.equal(body.reminders,false); assert.equal(body.log,false);
});

test('pageshow/visibility/online storms reuse a healthy socket and restore one lease timer', async () => {
  const h = browser(); h.connectRealtime(); const c = h.clients[0];
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
  const a = await h.signRealtimeWrite(h.device, 'command', { requestId: 'a', action: 'light_toggle' });
  const b = await h.signRealtimeWrite(h.device, 'command', { requestId: 'b', action: 'light_toggle' });
  const bodyA = JSON.parse(a.body), bodyB = JSON.parse(b.body);
  assert.equal(a.v, 2); assert.equal(bodyA.bootId, 123);
  assert.ok(bodyB.seq > bodyA.seq);
  assert.notEqual(bodyA.nonce, bodyB.nonce);
  assert.match(a.sig, /^[a-f0-9]{64}$/);
  h.controlSessions.get(h.device.id).expiresAt = 0;
  await assert.rejects(h.signRealtimeWrite(h.device, 'command', { requestId: 'c', action: 'light_toggle' }), /Đang chuẩn bị/);
  assert.equal(http, 0);
});

test('control grant preparation is single flight outside commands', async () => {
  const h = browser(); let http = 0, resolve;
  h.context.fetch = () => { http++; return new Promise(r => { resolve = r; }); };
  const a = h.refreshRealtimeSession(), b = h.refreshRealtimeSession();
  assert.equal(http, 1);
  resolve({ ok: true, status: 200, json: async () => ({ success: true,
    realtime: { url:'wss://test.invalid/realtime/browser/MAP-1234567890AB', ticket:'test-ticket' },
    control: { sessionKey: '07'.repeat(32), expiresAt: Math.floor(Date.now() / 1000) + 300,
      grant: 'test|grant', grantSig: '08'.repeat(32) } }) });
  assert.deepEqual(await Promise.all([a, b]), [true, true]);
  assert.equal(h.state.authRequests.size, 0);
});

test('brief hide resets resume coalescing and restores the foreground lease immediately', async () => {
  const h = browser(); h.connectRealtime(); const c = h.clients[0];
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
    realtime:{url:'wss://test.invalid/realtime/browser/MAP-1234567890AB',ticket:'test-ticket'} }) }; };
  await h.refreshRealtimeSession();
  assert.equal(h.state.realtimeSessionState, 'error');
  assert.ok(h.state.authRetryAt > Date.now());
  assert.equal(h.controlReady(h.device), false);
  h.handleSnapshot(h.device, sample()); h.prefetchControlSession();
  assert.equal(http, 1);
});

async function warmBrowser() {
  const h = browser({ sessionTtlMs:45000, connectTimeoutMs:15000, keepaliveSeconds:30,
    staleAfterMs:8000, offlineAfterMs:30000 });
  h.published = []; h.probes = []; h.connectRealtime();
  const c = h.clients[0];
  c.subscribe = (filters, options, cb) => {
    if (typeof filters === 'string') { h.probes.push({topic:filters, callback:cb}); return; }
    options(null, Object.entries(filters).map(([topic, x]) => ({topic, qos:x.qos})));
  };
  c.send = (topic, body, cb) => { h.published.push({topic,body}); cb?.(); };
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
    assert.equal((await h.signRealtimeWrite(h.device,'command',{requestId:'return',action:'light_toggle'})).v,2);
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
  assert.ok(h.clients[0].resumes>0);
  h.tick();
  assert.equal(h.clients.length,1); assert.equal(h.state.brokerProbe,undefined);
});

test('suspended browser reuses one transport owner; resume storms remain delegated', async () => {
  const h=await warmBrowser(); h.hide(); h.elapse(400000,0); h.foreground();
  for(let i=0;i<30;i++) h.resumeBrowserConnection({type:'online'});
  await new Promise(setImmediate); assert.equal(h.clients.length,1); assert.ok(h.clients[0].resumes>0);
  h.handlePresence(h.device,{online:false,bootId:123}); assert.equal(h.connectionStatus(h.device),'offline');
});
test('closed transport remains the sole reconnect owner after resume', async () => {
  const h=await warmBrowser(); h.hide(); h.clients[0].connected=false; h.clients[0].emit('close');
  h.elapse(400000); h.foreground(); for(let i=0;i<30;i++) h.resumeBrowserConnection({type:'online'});
  assert.equal(h.clients.length,1); h.clients[0].connected=true; h.clients[0].emit('connect');
  await new Promise(setImmediate); assert.equal(h.published.at(-1).body.active,true);
});

test('warm grant renewal is proactive/single-flight and stops in idle; expired clicks never do HTTP', async () => {
  const h=await warmBrowser(); let http=0, finish;
  h.context.fetch=()=>{http++; return new Promise(resolve=>{finish=resolve;});};
  h.controlSessions.get(h.device.id).expiresAt=Math.floor(h.now()/1000)+300;
  h.hide(); h.elapse(239000); h.tick(); h.prefetchControlSession(); assert.equal(http,0);
  h.elapse(2000); h.tick(); for(let i=0;i<20;i++) h.prefetchControlSession();
  assert.equal(http,1);
  finish({ok:true,status:200,json:async()=>({success:true,realtime:{url:'wss://test.invalid/realtime/browser/MAP-1234567890AB',ticket:'test-ticket'},
    control:{sessionKey:'07'.repeat(32),expiresAt:Math.floor(h.now()/1000)+300,grant:'renew|grant',grantSig:'08'.repeat(32)}})});
  await new Promise(setImmediate);
  h.elapse(60000); h.tick(); assert.equal(h.state.backgroundMode,'idle');
  h.controlSessions.get(h.device.id).expiresAt=0;
  h.prefetchControlSession(); assert.equal(http,1);
  await assert.rejects(h.signRealtimeWrite(h.device,'command',{requestId:'expired',action:'light_toggle'}),/Đang chuẩn bị/);
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

test('OS suspension liveness is delegated without changing controller state', async () => {
  const h=await warmBrowser(); h.hide(); h.elapse(400000,0); h.foreground(); await new Promise(setImmediate);
  assert.equal(h.clients.length,1); assert.ok(h.clients[0].resumes>0);
});

test('native ticket renewal and UI grant preparation share one HTTP request',async()=>{
 const h=browser();let calls=0,finish;
 h.context.fetch=()=>{calls++;return new Promise(resolve=>finish=resolve);};
 const native=h.requestRealtimeSession(h.device),ui=h.refreshRealtimeSession();assert.equal(calls,1);
 finish({ok:true,status:200,json:async()=>({success:true,realtime:{url:'wss://test.invalid/realtime/browser/MAP-1234567890AB',ticket:'one-use-ticket'},control:{sessionKey:'07'.repeat(32),grant:'shared|grant',grantSig:'08'.repeat(32),expiresAt:Math.floor(h.now()/1000)+300}})});
 assert.equal((await native).ticket,'one-use-ticket');assert.equal(await ui,true);assert.equal(calls,1);
});
