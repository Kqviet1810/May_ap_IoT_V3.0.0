// Real DOM + WebCrypto with an isolated broker/Cloud fixture. No machine I/O.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { chromium } = require(process.env.MAYAP_PLAYWRIGHT || 'playwright');
const root = path.resolve(__dirname, '..');
const out = path.resolve(process.argv[2] || path.join(root, 'work', 'connection-qa'));
fs.mkdirSync(out, { recursive: true });
const app = fs.readFileSync(path.join(root, 'app.js'), 'utf8').replace('  init();', `
  window.__qa = { state, showPage, controlReady, connectionStatus, handlePresence,
    handleSnapshot, handleBootstrap, REQUIRED_CONFIG_KEYS, VENT_PROFILE_KEYS,
    enterBackground, warmRemainingMs, checkBackgroundDeadline, resumeBrowserConnection,
    controlSessions, prefetchControlSession, renderDevice, renderPushStatus };
  init();`);
const defaults = {};
const config = fs.readFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v4_0_0/config.h'), 'utf8');
for (const m of config.matchAll(/\b(?:float|bool|uint8_t|uint16_t|uint32_t)\s+(\w+)\s*=\s*(true|false|\d+(?:\.\d+)?)(?:f|U|UL)?\s*;/g))
  defaults[m[1]] = m[2] === 'true' ? true : m[2] === 'false' ? false : Number(m[2]);
const transport = `
window.__transport = { connects: 0, subscriptions: [], sessions: [], commands: [], clients: [], holdLive: true };
window.mqtt = { connect(url, options) {
  const t = window.__transport, handlers = {};
  t.connects++;
  const client = { connected: false, disconnecting: false, options,
    on(name, fn) { (handlers[name] ||= []).push(fn); return client; },
    emit(name, ...args) { (handlers[name] || []).forEach(fn => fn(...args)); },
    deliver(suffix, body, retain = false) {
      client.emit('packetreceive', { cmd: 'publish' });
      client.emit('message', 'mayap/v1/MAP-1234567890AB/' + suffix, JSON.stringify(body), { retain });
    },
    snapshot() { client.deliver('snapshot', { bootId: 123, revision: 1,
      runtime: { temperature: 37.8, humidity: 59, batchRunning: true, machineState: 'DANG AP',
        currentDay: 7, heaterPower: 25, lightOn: true, circulationFanOn: true,
        turnState: 3, nextTurnMinutes: 52, activeFaults: [] } }); },
    subscribe(filters, options, callback) {
      const cb = typeof options === 'function' ? options : callback;
      if (typeof filters === 'string') filters = { [filters]: options };
      t.subscriptions.push(Object.keys(filters));
      setTimeout(() => {
        for (const topic of Object.keys(filters)) {
          if (topic.endsWith('/presence')) client.deliver('presence', { online: true, bootId: 123, proto: 2, fw: '4.0.0' }, true);
          if (topic.endsWith('/bootstrap')) client.deliver('bootstrap', { v: 1, proto: 2, bootId: 123, revision: 1,
            temperature: 37.4, humidity: 58, machineState: 'DANG AP', batchRunning: true,
            publishedAt: Math.floor(Date.now()/1000), faultCode: 0, humidifierInstalled: true }, true);
        }
        cb(null, Object.entries(filters).map(([topic, v]) => ({ topic, qos: v.qos })));
      }, 20);
    },
    unsubscribe() {}, reconnect() { t.manualReconnects = (t.manualReconnects || 0) + 1; },
    end() { client.disconnecting = true; client.connected = false; client.emit('close'); },
    publish(topic, wire, options, cb) {
      cb?.(); if (!wire) return;
      const body = JSON.parse(wire);
      if (topic.endsWith('/session')) {
        t.sessions.push(body);
        if (body.active && body.sync) setTimeout(() => {
          if (!t.holdLive) client.snapshot();
          if (body.config) {
            const h = window.__qa;
            const cfg = Object.fromEntries(h.REQUIRED_CONFIG_KEYS.concat(h.VENT_PROFILE_KEYS).map(k => [k, window.__defaults[k] ?? 0]));
            client.deliver('config/reported', { bootId: 123, revision: 1, config: cfg });
          }
          if (body.reminders) client.deliver('reminders/reported', { bootId: 123, revision: 1, reminders: [] }, true);
        }, 25);
      }
      if (topic.endsWith('/command')) (async () => {
        const key = await crypto.subtle.importKey('raw', new Uint8Array(32).fill(7), { name:'HMAC', hash:'SHA-256' }, false, ['sign', 'verify']);
        const enc = new TextEncoder(), payload = JSON.parse(body.body);
        const sig = new Uint8Array(body.sig.match(/../g).map(h=>parseInt(h,16)));
        const valid = await crypto.subtle.verify('HMAC', key, sig,
          enc.encode('mayap-mqtt-write:v2\\nMAP-1234567890AB\\ncommand\\n'+body.grant+'\\n'+body.body));
        t.commands.push({ valid, payload, qos: options.qos, retain: options.retain });
        for (const phase of ['received', 'completed']) {
          const ack = { v:2, requestId:payload.requestId, operation:payload.action.replaceAll('_','.'),
            phase, result:phase === 'received' ? 'accepted' : 'applied', ok:true,
            code:phase === 'received' ? 'RECEIVED' : 'APPLIED', bootId:123, revision:1, message:'' };
          const text = ['mayap-mqtt-ack:v2','MAP-1234567890AB',ack.requestId,ack.operation,ack.phase,'1',ack.code,123,1,''].join('\\n');
          ack.sig = Array.from(new Uint8Array(await crypto.subtle.sign('HMAC', key, enc.encode(text))), b=>b.toString(16).padStart(2,'0')).join('');
          client.deliver('ack', ack);
          await new Promise(r=>setTimeout(r,15));
        }
      })().catch(error=> { t.error = error.message; });
    }
  };
  t.clients.push(client);
  setTimeout(() => { client.connected = true; client.emit('packetreceive', {cmd:'connack'}); client.emit('connect'); },20);
  return client;
} };`;

async function main() {
  const executablePath = process.env.MAYAP_CHROME || (process.platform === 'win32' ? 'C:/Program Files/Google/Chrome/Application/chrome.exe' : undefined);
  const browser = await chromium.launch({ executablePath, headless: true });
  const results = [];
  try {
    for (const width of [390, 1440]) {
      const context = await browser.newContext({ viewport: { width, height: 844 }, serviceWorkers: 'block' });
      await context.addInitScript(defaults => {
        window.__defaults = defaults;
        sessionStorage.setItem('mayap.account.session.v1','aa'.repeat(32));
        const at = Date.now() - 60000;
        localStorage.setItem('mayap.web.v10.devices', JSON.stringify([{ id:'MAP-1234567890AB', name:'Máy thử', pairingToken:'qa-token' }]));
        localStorage.setItem('mayap.account.qa-sub.selected', 'MAP-1234567890AB');
        localStorage.setItem('mayap.account.qa-sub.runtime.v1.MAP-1234567890AB', JSON.stringify({ v:1, receivedAt:at,
          presence:{online:true,proto:2,bootId:123}, presenceAt:at,
          snapshot:{bootId:123,revision:1,runtime:{temperature:36.9,humidity:57,machineState:'DANG AP',batchRunning:true,lightOn:true,activeFaults:[{code:110,severity:1}]}} }));
      }, defaults);
      const requests = [], errors = [];
      let releaseAuth;
      const authGate = new Promise(r => { releaseAuth = r; });
      await context.route('**/*', async route => {
        const url = new URL(route.request().url());
        if(route.request().method()==='OPTIONS')return route.fulfill({headers:{'Access-Control-Allow-Origin':'http://127.0.0.1:8765'},status:204,headers:{'Access-Control-Allow-Origin':'http://127.0.0.1:8765','Access-Control-Allow-Headers':'authorization,content-type','Access-Control-Allow-Methods':'GET,POST,DELETE'}});
        if (url.origin === 'http://127.0.0.1:8765' && !url.pathname.startsWith('/api/')) {
          if (url.pathname === '/app.js') return route.fulfill({ contentType:'application/javascript', body:app });
          if (url.pathname === '/vendor/mqtt.min.js') return route.fulfill({ contentType:'application/javascript', body:transport });
          return route.continue();
        }
        requests.push(url.pathname);
        let body = { success:true };
        if (url.pathname === '/api/account/session') body = { success:true,
          user:{sub:'qa-sub',name:'QA account'},expiresAt:Date.now()+86400000,
          devices:[{device_id:'MAP-1234567890AB',device_name:'Máy thử',role:'owner'}] };
        if (url.pathname.endsWith('/mqtt-session')) {
          await authGate;
          body = { success:true, mqtt:{url:'wss://qa.invalid/mqtt',username:'qa',password:'qa'},
            control:{grant:'qa|grant',grantSig:'08'.repeat(32),sessionKey:'07'.repeat(32),expiresAt:Math.floor(Date.now()/1000)+300} };
        }
        if (url.pathname.endsWith('/firmware/latest')) body = { success:true, version:'4.0.0' };
        await route.fulfill({headers:{'Access-Control-Allow-Origin':'http://127.0.0.1:8765'}, status:200, contentType:'application/json', body:JSON.stringify(body) });
      });
      const page = await context.newPage();
      page.on('pageerror', error => errors.push(error.message));
      await page.goto('http://127.0.0.1:8765', { waitUntil:'domcontentloaded' });
      await page.waitForFunction(()=>document.documentElement.dataset.auth==='ready');
      assert.equal(await page.locator('#liveTemp').innerText(), '36,9°C');
      assert.equal(await page.locator('#onlinePill').innerText(), 'ĐANG ĐỒNG BỘ');
      assert.match(await page.locator('#dataFreshness').innerText(), /Lưu ·/);
      assert.match(await page.locator('#dataFreshness').getAttribute('title'), /Dữ liệu đã nhận/);
      assert.equal(await page.locator('#outputLightBtn').isDisabled(), true);
      assert.equal(await page.evaluate(()=>window.__transport.connects), 0, 'Cache is visible while HTTP auth is still pending');
      await page.screenshot({ path:path.join(out, `cache-${width}.png`) });
      releaseAuth();
      await page.waitForFunction(()=>window.__qa.state.devices[0].dataSource === 'bootstrap');
      assert.equal(await page.locator('#onlinePill').innerText(), 'ĐANG ĐỒNG BỘ');
      assert.match(await page.locator('#dataFreshness').innerText(), /Máy chủ ·/);
      assert.equal(await page.locator('#outputHumidifierTile').isVisible(),true,'Bootstrap exposes installed humidifier without loading full config');
      assert.deepEqual(await page.evaluate(()=>window.__transport.subscriptions[0].map(t=>t.split('/').at(-1))), ['presence','bootstrap','snapshot','ack']);
      await page.evaluate(()=> { window.__transport.holdLive=false; window.__transport.clients[0].snapshot(); });
      await page.waitForFunction(()=>document.body.dataset.connection === 'online' && window.__qa.controlReady(window.__qa.state.devices[0]));
      assert.equal(await page.locator('#liveTemp').innerText(), '37,8°C');
      assert.equal(await page.locator('#outputLightBtn').isEnabled(), true);
      const httpBefore = requests.length;
      await page.locator('#outputLightBtn').click();
      await page.waitForFunction(()=>window.__transport.commands.length === 1 && window.__qa.state.pending.size === 0);
      assert.equal(requests.length, httpBefore, 'No HTTP per command');
      const command = await page.evaluate(()=>window.__transport.commands[0]);
      assert.ok(command.valid); assert.equal(command.qos,1); assert.equal(command.retain,false);
      assert.equal(command.payload.bootId,123); assert.ok(command.payload.expiresAt); assert.ok(command.payload.seq);
      const resume = await page.evaluate(()=> {
        const start=performance.now();
        for(let i=0;i<30;i++) { window.dispatchEvent(new Event('pageshow')); window.dispatchEvent(new Event('online')); document.dispatchEvent(new Event('visibilitychange')); }
        return { duration:performance.now()-start, connects:window.__transport.connects };
      });
      assert.equal(resume.connects,1); assert.ok(resume.duration < 500);
      assert.equal(await page.evaluate(()=>window.__transport.manualReconnects || 0),0);
      // Real DOM lifecycle with elapsed timestamps, without waiting five minutes
      // or sending commands to a physical device. OS timer freeze is unit tested.
      await page.evaluate(()=> {
        window.__qaHidden=false;
        Object.defineProperty(document,'hidden',{configurable:true,get:()=>window.__qaHidden});
      });
      for(const seconds of [30,120,179,180,299]) {
        const warm=await page.evaluate(seconds=> {
          const h=window.__qa;
          window.__qaHidden=true; document.dispatchEvent(new Event('visibilitychange'));
          h.state.hiddenAt-=seconds*1000; h.state.hiddenMonoAt-=seconds*1000;
          const result={mode:h.state.backgroundMode,remaining:h.warmRemainingMs(),active:window.__transport.sessions.at(-1).active};
          window.__qaHidden=false; document.dispatchEvent(new Event('visibilitychange'));
          return result;
        },seconds);
        assert.equal(warm.mode,'warm'); assert.ok(warm.remaining>0); assert.equal(warm.active,true);
        assert.equal(await page.evaluate(()=>window.__transport.connects),1);
      }
      const idle=await page.evaluate(()=> {
        const h=window.__qa;
        window.__qaHidden=true; document.dispatchEvent(new Event('visibilitychange'));
        h.state.hiddenAt-=300000; h.checkBackgroundDeadline();
        return {mode:h.state.backgroundMode,timer:h.state.sessionTimer,active:window.__transport.sessions.at(-1).active,connects:window.__transport.connects};
      });
      assert.deepEqual(idle,{mode:'idle',timer:0,active:false,connects:1});
      await page.evaluate(async()=> {
        window.__qaHidden=false; document.dispatchEvent(new Event('visibilitychange'));
        // Settle the existing independent push/status refresh before measuring
        // command HTTP. A command must never provision/sign through Cloudflare.
        await window.__qa.renderPushStatus();
      });
      const returnHttp=requests.length;
      await page.locator('#outputLightBtn').click();
      await page.waitForFunction(()=>window.__transport.commands.length===2 && window.__qa.state.pending.size===0);
      assert.equal(requests.length,returnHttp,JSON.stringify(requests.slice(returnHttp)));
      assert.equal(await page.evaluate(()=>window.__transport.connects),1);
      let allowRenew;
      const renewGate=new Promise(resolve=>{allowRenew=resolve;});
      await context.route('**/api/device/mqtt-session',async route=> {
        requests.push(new URL(route.request().url()).pathname);
        await renewGate;
        await route.fulfill({headers:{'Access-Control-Allow-Origin':'http://127.0.0.1:8765'},status:200,contentType:'application/json',body:JSON.stringify({success:true,
          mqtt:{url:'wss://qa.invalid/mqtt',username:'qa',password:'qa'},
          control:{grant:'qa|renew',grantSig:'08'.repeat(32),sessionKey:'07'.repeat(32),expiresAt:Math.floor(Date.now()/1000)+300}})});
      });
      await page.evaluate(()=> {
        const h=window.__qa;
        h.controlSessions.get(h.state.selectedId).expiresAt=0;
        h.state.devices[0].snapshotAt=Date.now()-40000; h.renderDevice();
      });
      assert.match(await page.locator('#wifiConnectionText').innerText(),/Đang chuẩn bị quyền điều khiển/);
      assert.match(await page.locator('#wifiConnectionText').innerText(),/Chờ dữ liệu/);
      assert.equal(await page.locator('#outputLightBtn').isDisabled(),true);
      await page.evaluate(()=>window.__qa.prefetchControlSession());
      await page.waitForFunction(()=>window.__qa.state.authRequests.size===1);
      await page.evaluate(()=>window.__transport.clients[0].snapshot());
      allowRenew();
      await page.waitForFunction(()=>window.__qa.controlReady(window.__qa.state.devices[0]));
      assert.equal(await page.locator('#outputLightBtn').isEnabled(),true);
      assert.equal(await page.evaluate(()=>window.__transport.connects),1);
      await page.evaluate(()=>window.__qa.showPage('settings'));
      await page.waitForFunction(()=>window.__qa.state.devices[0].configAt > 0);
      assert.ok(await page.evaluate(()=>window.__transport.subscriptions.flat().some(t=>t.endsWith('/config/reported'))));
      assert.equal(await page.evaluate(()=>window.__transport.subscriptions.flat().some(t=>t.endsWith('/history/reported') || t.endsWith('/reminders/reported') || t.endsWith('/log'))), false);
      await page.evaluate(()=>document.getElementById('remindersForm').closest('details').open=true);
      await page.waitForFunction(()=>window.__qa.state.devices[0].remindersLoaded);
      await page.evaluate(()=>window.__qa.showPage('device'));
      await page.evaluate(()=>window.__qa.handlePresence(window.__qa.state.devices[0],{online:false,bootId:123}));
      assert.equal(await page.locator('#onlinePill').innerText(), 'MÁY NGOẠI TUYẾN');
      await page.evaluate(()=> {
        const h=window.__qa,d=h.state.devices[0]; h.handlePresence(d,{online:true,bootId:123,proto:2});
        d.snapshotAt=Date.now()-40000; h.handleBootstrap(d,{v:1,bootId:1,temperature:99,machineState:'OLD'});
        window.dispatchEvent(new Event('pageshow'));
      });
      assert.equal(await page.locator('#onlinePill').innerText(),'DỮ LIỆU CHẬM');
      assert.equal(await page.locator('#outputLightBtn').isDisabled(), true);
      assert.deepEqual(errors,[]);
      assert.equal(await page.evaluate(()=>Storage.prototype.getItem.call(localStorage,'mayap.account.qa-sub.runtime.v1.MAP-1234567890AB') !== null),true);
      await page.screenshot({ path:path.join(out, `degraded-${width}.png`) });
      results.push({ width, cacheBeforeAuth:true, bootstrapRetained:true, snapshotSupersedes:true,
        lazy:true, signedCommandAndAck:true, noClickHttp:true, brokerReuse:true,
        simulatedResumeMs:Math.round(resume.duration), warm300s:true, idleRetainsSocket:true,
        warmReturnNoClickHttp:true, expiredGrantUi:true, proactiveGrantRenew:true, offlineAndDegraded:true, errors });
      await context.close();
    }
    // Exercise the actual service worker and public config.js offline, without
    // provisioning or connecting to any real broker/device.
    const pwa = await browser.newContext({ viewport:{width:390,height:844}, serviceWorkers:'allow' });
    await pwa.route('**/*', route => {
      const url = new URL(route.request().url());
        if(route.request().method()==='OPTIONS')return route.fulfill({headers:{'Access-Control-Allow-Origin':'http://127.0.0.1:8765'},status:204,headers:{'Access-Control-Allow-Origin':'http://127.0.0.1:8765','Access-Control-Allow-Headers':'authorization,content-type','Access-Control-Allow-Methods':'GET,POST,DELETE'}});
      if (url.pathname === '/api/account/session') return route.fulfill({headers:{'Access-Control-Allow-Origin':'http://127.0.0.1:8765'},status:200,contentType:'application/json',body:JSON.stringify({
        success:true,user:{sub:'qa-sub',name:'QA'},devices:[],expiresAt:Date.now()+86400000})});
      return url.origin === 'http://127.0.0.1:8765' ? route.continue() : route.fulfill({headers:{'Access-Control-Allow-Origin':'http://127.0.0.1:8765'},status:503,contentType:'application/json',body:'{"success":false}'});
    });
    await pwa.addInitScript(()=>sessionStorage.setItem('mayap.account.session.v1','aa'.repeat(32)));
    const offlinePage = await pwa.newPage();
    const offlineErrors = [];
    offlinePage.on('pageerror', error => offlineErrors.push(error.message));
    await offlinePage.goto('http://127.0.0.1:8765', {waitUntil:'domcontentloaded'});
    await offlinePage.waitForFunction(()=>Boolean(navigator.serviceWorker.controller));
    await offlinePage.evaluate(()=> {
      localStorage.setItem('mayap.web.v10.devices',JSON.stringify([{id:'MAP-1234567890AB',name:'Máy lưu',pairingToken:'offline-fixture'}]));
      localStorage.setItem('mayap.account.qa-sub.selected','MAP-1234567890AB');
      localStorage.setItem('mayap.account.qa-sub.runtime.v1.MAP-1234567890AB',JSON.stringify({v:1,receivedAt:Date.now()-60000,
        snapshot:{bootId:123,revision:1,runtime:{temperature:36.9,humidity:57,batchRunning:true,activeFaults:[]}},presence:{online:true,proto:2}}));
    });
    await pwa.unroute('**/*'); // Route.fulfill bypasses browser offline emulation.
    await pwa.setOffline(true);
    await offlinePage.reload({waitUntil:'domcontentloaded'});
    assert.equal(await offlinePage.locator('.appShell').isVisible(),false,'Cold offline reload cannot trust cached ownership');
    assert.equal(await offlinePage.locator('.landing').isVisible(),false,'Network failure is not logout');
    assert.equal(await offlinePage.locator('.authLoading').isVisible(),true);
    assert.equal(await offlinePage.evaluate(()=>window.MAYAP_WEB_CONFIG.keepaliveSeconds),30,'Offline config hardening still runs');
    assert.deepEqual(offlineErrors,[]);
    await offlinePage.screenshot({path:path.join(out,'pwa-offline-cache.png')});
    results.push({pwaOffline:true,actualServiceWorker:true,publicConfigLoaded:true,accountGateFailsClosed:true,errors:offlineErrors});
    await pwa.close();
    fs.writeFileSync(path.join(out,'connection-browser-qa.json'),JSON.stringify({ passed:true, transport:'isolated fixture + actual offline service worker', results },null,2));
    console.log(JSON.stringify(results));
  } finally { await browser.close(); }
}
main().catch(error=> { console.error(error); process.exitCode=1; });
