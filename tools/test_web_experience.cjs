// Browser QA with isolated mock transport. Never connects to a real machine.
// Usage: MAYAP_PLAYWRIGHT=<package path> node tools/test_web_experience.cjs <output-dir>
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { chromium } = require(process.env.MAYAP_PLAYWRIGHT || 'playwright');
const root = path.resolve(__dirname, '..');
const out = path.resolve(process.argv[2] || path.join(root, 'work', 'web-qa'));
fs.mkdirSync(out, { recursive: true });
const source = fs.readFileSync(path.join(root, 'app.js'), 'utf8').replace('  init();', `
  window.__qa = { state, REQUIRED_CONFIG_KEYS, VENT_PROFILE_KEYS, handleConfigReport,
    handleSnapshot, handlePresence, showPage, buildConfig, createDevice, renderSelector, renderDevice, connectionStatus };
  init();`);
const firmware = fs.readFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v4_0_0/config.h'), 'utf8');
const defaults = {};
for (const match of firmware.matchAll(/\b(?:float|bool|uint8_t|uint16_t|uint32_t)\s+(\w+)\s*=\s*(true|false|\d+(?:\.\d+)?)(?:f|U|UL)?\s*;/g))
  defaults[match[1]] = match[2] === 'true' ? true : match[2] === 'false' ? false : Number(match[2]);
const fakeMqtt = `
window.__transport = { subscriptions: [], sessions: [], connects: 0, dropFirst: false };
window.mqtt = { connect() {
  window.__transport.connects++;
  const handlers = {}, client = { connected: false, disconnecting: false,
    on(name, fn) { (handlers[name] ||= []).push(fn); return client; },
    emit(name, ...args) { (handlers[name] || []).forEach(fn => fn(...args)); },
    subscribe(filters, cb) { window.__transport.subscriptions.push(filters);
      setTimeout(() => cb(null, Object.entries(filters).map(([topic, v]) => ({ topic, qos: v.qos }))), 20); },
    unsubscribe() {}, reconnect() { client.emit('connect'); },
    end() { client.disconnecting = true; client.emit('close'); },
    publish(topic, wire, options, cb) {
      cb?.(); if (!topic.endsWith('/session') || !wire) return;
      const msg = JSON.parse(wire); window.__transport.sessions.push({ at: performance.now(), ...msg });
      if (!msg.active || !msg.sync) return;
      if (window.__transport.dropFirst) { window.__transport.dropFirst = false; return; }
      setTimeout(() => window.__deliver?.(), 25);
    }
  };
  setTimeout(() => { client.connected = true; client.emit('connect'); }, 20);
  return client;
} };`;

async function setup(browser, options = {}) {
  let authFailures = options.authFailures || 0;
  const context = await browser.newContext({ viewport: { width: options.width || 390, height: options.height || 844 },
    isMobile: Boolean(options.mobile), hasTouch: Boolean(options.mobile), serviceWorkers: 'block', colorScheme: options.scheme || 'light' });
  await context.addInitScript(({ theme, paired, defaults, dropFirst }) => {
    window.__nativeStorageGet = Storage.prototype.getItem;
    if (theme) localStorage.setItem('mayap.theme', theme);
    if (paired) {
      localStorage.setItem('mayap.web.v10.devices', JSON.stringify([{ id: 'MAP-1234567890AB', name: 'Máy ấp nhà mình', pairingToken: 'qa-token' }]));
      localStorage.setItem('mayap.web.v10.selected', 'MAP-1234567890AB');
    }
    window.__deliver = () => {
      const h = window.__qa, d = h.state.devices[0]; if (!d) return;
      const config = Object.fromEntries(h.REQUIRED_CONFIG_KEYS.concat(h.VENT_PROFILE_KEYS).map(key => [key, defaults[key] ?? 0]));
      config.ventAutoEnabled = true;
      h.handlePresence(d, { online: true, bootId: 123, fw: '4.0.0', proto: 2, ssid: 'Wi-Fi gia đình' });
      h.handleConfigReport(d, { revision: 1, bootId: 123, config });
      h.handleSnapshot(d, { revision: 1, bootId: 123, runtime: { temperature: 37.5, humidity: 58,
        heaterPower: 25, circulationFanOn: true, ventFanOn: false, turningEnabled: true,
        batchRunning: true, currentDay: 7, turnState: 3, nextTurnMinutes: 52, faults: [] } });
    };
    document.addEventListener('DOMContentLoaded', () => { window.__transport.dropFirst = dropFirst; });
  }, { theme: options.theme, paired: options.paired !== false, defaults, dropFirst: Boolean(options.dropFirst) });
  await context.route('**/*', async route => {
    const url = new URL(route.request().url());
    if (url.origin === 'http://127.0.0.1:8765') {
      if (url.pathname === '/app.js') return route.fulfill({ contentType: 'application/javascript', body: source });
      if (url.pathname === '/vendor/mqtt.min.js') return route.fulfill({ contentType: 'application/javascript', body: fakeMqtt });
      return route.continue();
    }
    let body = { success: false, error: 'QA fixture' };
    if (url.pathname.endsWith('/mqtt-session') && authFailures-- > 0)
      return route.fulfill({status:503,contentType:'application/json',body:JSON.stringify(body)});
    if (url.pathname.endsWith('/mqtt-session') || url.pathname.endsWith('/verify-pin')) body = {
      success: true, pairing_token: 'qa-token', device_name: 'Máy ấp nhà mình',
      mqtt: { url: 'wss://qa.invalid/mqtt', username: 'qa', password: 'qa' },
      control: { sessionId: 'qa-session', sessionKey: '07'.repeat(32), grant: 'qa|grant', grantSig: '08'.repeat(32), expiresAt: Math.floor(Date.now()/1000)+300 } };
    if (url.pathname.endsWith('/firmware/latest')) body = { success: true, version: '4.0.0' };
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
  });
  const page = await context.newPage(), errors = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.goto('http://127.0.0.1:8765', { waitUntil: 'networkidle' });
  if (options.paired !== false) await page.waitForFunction(() => window.__qa.state.devices[0]?.snapshotAt);
  assert.deepEqual(errors, [], 'Browser exceptions');
  return { context, page, errors };
}

async function swipe(page, x, y, dx, dy = 0) {
  const cdp = await page.context().newCDPSession(page);
  await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x, y }] });
  for (let i=1; i<=6; ++i) await cdp.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ x:x+dx*i/6, y:y+dy*i/6 }] });
  await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
  // Tab commitment must be immediate at release, before transition finishes.
  // Tests below separately wait for animation completion before a new gesture.
  await page.waitForFunction(() => !document.querySelector('.page[inert]') &&
    !document.getAnimations().some(animation=>animation.playState==='running'));
  await cdp.detach();
}

async function inspectControlLayout(page) {
  return page.evaluate(() => {
    const savedPage = document.body.dataset.page;
    const details = [...document.querySelectorAll('#page-settings details.settingCard')];
    const savedOpen = details.map(el => el.open);
    const style = document.createElement('style');
    style.textContent = '.toggle i,.toggle i::after,.switchRow i,.switchRow i::after{transition:none!important}';
    document.head.append(style);
    const toggles = [];
    try {
      details.forEach(el => { el.open = true; });
      for (const tab of ['batch', 'settings']) {
        window.__qa.showPage(tab);
        for (const input of document.querySelectorAll(`#page-${tab} .toggle input`)) {
          const track = input.nextElementSibling, box = track.getBoundingClientRect();
          if (!box.width || !box.height) continue;
          const savedChecked = input.checked;
          try {
            for (const checked of [false, true]) {
              input.checked = checked;
              const outer = getComputedStyle(track), thumb = getComputedStyle(track, '::after');
              const matrix = new DOMMatrixReadOnly(thumb.transform);
              const n = value => parseFloat(value) || 0;
              const thumbWidth = n(thumb.width) + (thumb.boxSizing==='border-box' ? 0 : n(thumb.borderLeftWidth)+n(thumb.borderRightWidth));
              const thumbHeight = n(thumb.height) + (thumb.boxSizing==='border-box' ? 0 : n(thumb.borderTopWidth)+n(thumb.borderBottomWidth));
              const left = n(outer.borderLeftWidth)+n(thumb.left)+matrix.m41;
              const top = n(outer.borderTopWidth)+n(thumb.top)+matrix.m42;
              const labelBox = input.parentElement.getBoundingClientRect();
              toggles.push({id:input.id,checked,width:box.width,height:box.height,
                verticalError:Math.abs(top+thumbHeight/2-box.height/2),
                edgeGap:checked ? box.width-left-thumbWidth : left,
                labelHeight:labelBox.height, thumbBorder:n(thumb.borderLeftWidth)});
            }
          } finally { input.checked = savedChecked; }
        }
      }
      const svg = document.querySelector('.nav button[data-page="settings"] svg');
      const gear = svg.getBBox(), view = svg.viewBox.baseVal;
      const gearBox = svg.getBoundingClientRect(), buttonBox = svg.closest('button').getBoundingClientRect();
      const gearReport = {centerErrorX:Math.abs(gear.x+gear.width/2-(view.x+view.width/2)),
        centerErrorY:Math.abs(gear.y+gear.height/2-(view.y+view.height/2)),
        inside:gear.x>=view.x+1 && gear.y>=view.y+1 && gear.x+gear.width<=view.x+view.width-1 && gear.y+gear.height<=view.y+view.height-1,
        mobileCenterError:Math.abs(gearBox.x+gearBox.width/2-buttonBox.x-buttonBox.width/2)};
      const hintIcons = [...document.querySelectorAll('#page-settings .settingHintTrigger,#page-settings .settingHintNote summary')]
        .filter(el => !['none','normal'].includes(getComputedStyle(el,'::before').content)).length;
      const line = getComputedStyle(document.documentElement).getPropertyValue('--line').trim();
      const swatch = document.createElement('div'); swatch.style.borderColor=line; document.body.append(swatch);
      const expectedBorder = getComputedStyle(swatch).borderTopColor; swatch.remove();
      const mismatchedBorders = [...document.querySelectorAll('.primary:not(.attention),.addDevice,.ghost,input:not([type="checkbox"]):not([type="radio"]):not(.customSelectNative)')]
        .filter(el => { const css=getComputedStyle(el); return nBorder(css.borderTopWidth)>0 && css.borderTopColor!==expectedBorder; })
        .map(el => el.id || el.className);
      function nBorder(value) { return parseFloat(value) || 0; }
      return {toggles,gear:gearReport,hintIcons,mismatchedBorders};
    } finally {
      details.forEach((el,i) => { el.open=savedOpen[i]; });
      window.__qa.showPage(savedPage);
      style.remove();
    }
  });
}

async function inspectHeader(page) {
  return page.evaluate(() => {
    const title = document.querySelector('.title').getBoundingClientRect();
    const readings = document.querySelector('.headerReadings').getBoundingClientRect();
    const fields = [...document.querySelectorAll('.live')].map(el => {
      const rect = el.getBoundingClientRect();
      return {x:rect.x,y:rect.y,width:rect.width,height:rect.height,
        labelY:el.firstElementChild.getBoundingClientRect().y};
    });
    return {titleTop:title.top,titleBottom:title.bottom,readingsLeft:readings.left,readingsWidth:readings.width,readingsTop:readings.top,readingsHeight:readings.height,fields,
      clipped:[...document.querySelectorAll('.title h1,.title p,.live>span,.live>strong')]
        .filter(el => el.scrollWidth>el.clientWidth+1).map(el => el.id || el.textContent),
      rows:getComputedStyle(document.querySelector('.topbar')).gridTemplateRows.split(' ').length};
  });
}

async function checkHeaderStates(page, {width,height,theme}) {
  await page.evaluate(() => window.__qa.showPage('device'));
  const baseline = await inspectHeader(page), result = [];
  for (const scenario of ['running','offline','warning','stop','emergency']) {
    await page.evaluate(scenario => {
      window.__deliver();
      const h=window.__qa,d=h.state.devices[0];
      if (scenario==='offline') { d.presence.online=false; h.renderDevice(); return; }
      const fault = {warning:{code:502,severity:1},stop:{code:301,severity:2},emergency:{code:112,severity:3}}[scenario];
      if (fault) h.handleSnapshot(d,{...d.snapshot,runtime:{...d.snapshot.runtime,
        activeFaults:[fault,{code:306,severity:1}]}});
    },scenario);
    const layout = await inspectHeader(page);
    assert.deepEqual(layout.clipped,[],`${width}x${height}/${theme}/${scenario}: header text fits`);
    assert.ok(Math.abs(layout.readingsHeight-baseline.readingsHeight)<=.5,'Fault states must not change strip height');
    if (scenario==='offline') assert.equal(await page.locator('#liveState').innerText(),'Ngoại tuyến');
    if (scenario==='running') assert.equal(await page.locator('#liveState').innerText(),'Đang ấp');
    if (width===390) await page.locator('.topbar').screenshot({path:path.join(out,`MAYAP-header-${scenario}-${theme}.png`)});
    if (['warning','stop','emergency'].includes(scenario)) {
      await page.locator('#liveStateTile').click();
      await page.locator('#faultPopup').waitFor({state:'visible'});
      assert.equal(await page.locator('.faultPopupItem').count(),2,'Every active fault is still shown');
      const popup = await page.locator('#faultPopup').evaluate(el => {
        const box=el.getBoundingClientRect();
        const point=document.elementFromPoint(box.x+8,box.y+box.height-8);
        return {inViewport:box.x>=0 && box.right<=innerWidth && box.y>=0 && box.bottom<=innerHeight,
          unobscured:point===el || el.contains(point)};
      });
      assert.ok(popup.inViewport && popup.unobscured,`${width}x${height}/${theme}/${scenario}: fault popup is accessible outside strip`);
      if (width===390 && scenario==='emergency') await page.screenshot({path:path.join(out,`MAYAP-fault-popup-${theme}.png`)});
      await page.locator('.title').click();
      assert.equal(await page.locator('#faultPopup').isVisible(),false);
    }
    result.push(scenario);
  }
  await page.evaluate(() => window.__deliver());
  return result;
}

async function main() {
  const executablePath = process.env.MAYAP_CHROME || (process.platform==='win32' ? 'C:/Program Files/Google/Chrome/Application/chrome.exe' : undefined);
  const browser = await chromium.launch({ executablePath, headless: true });
  const results = [], palettes = [], controls = [], headers = [], layouts = [], safeAreas = [];
  try {
    const viewports = [320,390,430,768,850,1440].map(width=>({width,height:844})).concat([
      {width:390,height:667},{width:1025,height:768},{width:1719,height:600},{width:1734,height:965},
      {width:844,height:390},{width:320,height:568}]);
    for (const {width,height} of viewports) for (const theme of ['light', 'dark']) {
      const { context, page, errors } = await setup(browser, { width, height, mobile: width < 800 || height<500, theme });
      for (const tab of ['device', 'batch', 'settings']) {
        await page.evaluate(tab => window.__qa.showPage(tab), tab);
        await page.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));
        const overflow = await page.evaluate(() => document.documentElement.scrollWidth > innerWidth);
        assert.equal(overflow, false, `${width}/${theme}/${tab} horizontal overflow`);
        const header = await inspectHeader(page);
        assert.equal(header.rows,1,`${width}x${height}/${tab}: title and readings share one row`);
        assert.ok(header.titleTop<header.readingsTop+header.readingsHeight && header.titleBottom>header.readingsTop,'Title aligns beside readings');
        assert.deepEqual(header.clipped,[],`${width}x${height}/${theme}/${tab}: title and readings fit`);
        assert.ok(Math.max(...header.fields.map(f=>f.labelY))-Math.min(...header.fields.map(f=>f.labelY))<=.5,'Reading labels align');
        if (tab!=='settings') {
          const layout = await page.evaluate(() => {
            const main=document.querySelector('.main'), chart=document.querySelector('.telemetryPanel').getBoundingClientRect();
            const canvas=document.querySelector('.telemetryCanvasWrap').getBoundingClientRect();
            const wrap=document.querySelector('.telemetryCanvasWrap'), bitmap=document.querySelector('#temperatureChartCanvas'),dpr=Math.max(1,Math.min(2,devicePixelRatio));
            return {scroll:main.scrollHeight-main.clientHeight,documentScroll:document.documentElement.scrollHeight-innerHeight,
              bitmapHeight:bitmap.height/dpr,canvasInnerHeight:wrap.clientHeight,
              chartLeft:chart.left,chartWidth:chart.width,canvasHeight:canvas.height};
          });
          const shouldFit = height>=667;
          if (shouldFit) assert.ok(layout.scroll<=1 && layout.documentScroll<=1,`${width}x${height}/${theme}/${tab}: available screen fits without scrolling ${JSON.stringify(layout)}`);
          if (tab==='batch') {
            assert.ok(layout.canvasHeight>=60,'Temperature chart remains visible');
            assert.ok(Math.abs(layout.bitmapHeight-layout.canvasInnerHeight)<=1,'Chart bitmap matches available height, keeping labels readable');
            if (width>=1025 && height>500) {
              assert.ok(Math.abs(layout.chartLeft-header.readingsLeft)<=.5 && Math.abs(layout.chartWidth-header.readingsWidth)<=.5,'Desktop readings match chart column');
              const batch=await page.evaluate(()=>{
                const form=document.getElementById('batchForm').getBoundingClientRect(),buttons=document.querySelector('.batchActions').getBoundingClientRect();
                const chart=document.querySelector('.telemetryPanel').getBoundingClientRect();
                return {formBottom:form.bottom,buttonsBottom:buttons.bottom,chartBottom:chart.bottom};
              });
              assert.ok(batch.formBottom-batch.buttonsBottom<=25,'Batch form ends after its buttons without filling to screen bottom');
              assert.ok(Math.abs(batch.formBottom-batch.chartBottom)<=.5,'Both desktop batch cards have the same bottom edge');
            }
          }
          if (tab==='device' && (width<=800 || height<=500)) {
            const operating=await page.evaluate(()=>{
              const cells=[...document.querySelectorAll('.quickField,.outputStrip>div,.outputStrip>button')]
                .map(el=>el.getBoundingClientRect()).filter(r=>r.width>0 && r.height>0);
              const note=document.getElementById('pageSubtitle'),form=document.getElementById('quickForm').getBoundingClientRect();
              return {heights:cells.map(r=>r.height),noteVisible:getComputedStyle(note).display!=='none' && note.getBoundingClientRect().height>0,
                note:note.textContent,navGap:document.querySelector('.nav').getBoundingClientRect().top-form.bottom,
                hiddenHumidifier:document.getElementById('outputHumidifierTile').getBoundingClientRect().height};
            });
            assert.equal(operating.heights.length,8,'Eight standard operating cells, optional humidifier remains hidden');
            assert.equal(operating.hiddenHumidifier,0);
            assert.ok(Math.max(...operating.heights)-Math.min(...operating.heights)<=.5,'All eight operating cells have equal height');
            assert.ok(operating.noteVisible && operating.note==='Theo dõi và điều khiển máy.','Mobile device subtitle is visible');
            if (shouldFit) assert.ok(operating.navGap>=8 && operating.navGap<=12,'Operating card ends close to the tab bar with a safe gap');
          }
          layouts.push({width,height,theme,tab,shouldFit,...layout});
        }
        if ((width<=800 || height<=500) && tab!=='settings') {
          const action=page.locator(tab==='device' ? '#quickForm button[type="submit"]' : '#batchAction');
          if (tab==='device' && width>=390 && width<=430 && height>=844) {
            assert.ok(await page.locator('#quickForm').evaluate(el=>
              el.getBoundingClientRect().bottom<=document.querySelector('.nav').getBoundingClientRect().top-8),
              'Typical mobile: the entire control card clears the bottom navigation before scrolling');
          }
          await action.evaluate(el=>el.scrollIntoView({block:'center',behavior:'instant'}));
          const accessible=await action.evaluate(el=>{
            const box=el.getBoundingClientRect();
            const points=[box.y+4,box.y+box.height/2,box.bottom-4].map(y=>{
              const hit=document.elementFromPoint(box.x+box.width/2,y);
              return {y,visible:hit===el || el.contains(hit),hit:hit?.className};
            });
            const main=document.querySelector('.main');
            return {ok:points.every(point=>point.visible),points,top:main.scrollTop,scroll:main.scrollHeight,height:main.clientHeight};
          });
          assert.ok(accessible.ok,`${width}x${height}/${theme}/${tab}: action remains reachable ${JSON.stringify(accessible)}`);
          await page.evaluate(()=>{document.querySelector('.main').scrollTop=0;window.scrollTo({top:0,behavior:'instant'});});
          await page.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));
          assert.ok(await page.locator('.headerReadings').evaluate(el=>{
            const box=el.getBoundingClientRect(),hit=document.elementFromPoint(box.x+20,box.y+box.height/2);
            return hit===el || el.contains(hit);
          }),'Readings remain visible after scrolling back to the top');
        }
        const gradientElements = await page.evaluate(() => [...document.querySelectorAll('*')]
          .filter(el => /gradient\(/i.test(getComputedStyle(el).backgroundImage))
          .map(el => el.id || el.className || el.tagName));
        assert.deepEqual(gradientElements, [], `${width}/${theme}/${tab}: backgrounds must be solid`);
        if ((width === 390 || width === 1440 || width===1719 || width===1734) && tab !== 'settings')
          // Mobile pages scroll inside .main. Capture its actual viewport:
          // Chrome full-document capture can mispaint nested sticky headers.
          await page.screenshot({path:path.join(out, `MAYAP-${tab}-${width}x${height}-${theme}.png`),fullPage:width>800 && height>500,animations:'disabled'});
      }
      assert.equal(await page.title(), 'MAYAP · Máy ấp trứng');
      const contrast = await page.evaluate(() => {
        const style=getComputedStyle(document.documentElement), canvas=document.createElement('canvas'), ctx=canvas.getContext('2d');
        function luminance(color) { ctx.fillStyle=color;ctx.fillRect(0,0,1,1);const rgb=ctx.getImageData(0,0,1,1).data;
          return Array.from(rgb).slice(0,3).map(x=>x/255).map(x=>x<=.04045 ? x/12.92 : ((x+.055)/1.055)**2.4)
            .reduce((sum,x,i)=>sum+x*[.2126,.7152,.0722][i],0); }
        const action = getComputedStyle(document.querySelector('.primary:not(.attention)'));
        const value=name=>name==='white' ? '#fff' : name==='actionText' ? action.color : name==='actionBg' ? action.backgroundColor : style.getPropertyValue('--'+name).trim();
        return [['ink','surface'],['muted','surface'],['label','tile'],['primary2','primarySoft'],['actionText','actionBg'],['navText','navBg'],['navActiveText','navActive'],['chartLine','surface'],['faint','tile'],['white','dangerAction'],['white','attentionBg']]
          .map(([fg,bg])=>{const a=luminance(value(fg)),b=luminance(value(bg));return {fg,bg,ratio:(Math.max(a,b)+.05)/(Math.min(a,b)+.05)};});
      });
      for (const pair of contrast) assert.ok(pair.ratio>=4.5, `${theme}: ${pair.fg}/${pair.bg} contrast ${pair.ratio}`);
      if (width===390) palettes.push({theme,contrast});
      assert.equal(await page.locator('#ventSettingCard').isVisible(), true);
      assert.equal(await page.locator('#ventProfileFields').evaluate(el => el.disabled), false);
      assert.equal(await page.locator('#sensorForm').evaluate(el => el.closest('.settingGroup').getAttribute('aria-labelledby')), 'settings-temperature');
      assert.equal(await page.locator('#ventForm').evaluate(el => el.closest('.settingGroup').getAttribute('aria-labelledby')), 'settings-ventilation');
      assert.equal(await page.locator('#wifiForm').innerText().then(text => /ESP32|HMI/.test(text)), false);
      if (width === 390 || width === 1440) {
        await page.evaluate(() => window.scrollTo({ top:0, behavior:'instant' }));
        await page.screenshot({ path: path.join(out, `MAYAP-Web-${width}-${theme}.png`), fullPage: true });
        await page.locator('#ventSettingCard > summary').click();
        await page.locator('#ventSettingCard .settingHintNote > summary').click();
        await page.evaluate(() => window.scrollTo({ top:0, behavior:'instant' }));
        await page.screenshot({ path: path.join(out, `MAYAP-Quạt-hút-${width}-${theme}.png`), fullPage: true });
        if (width === 390) {
          await page.evaluate(() => { const box=document.getElementById('ventSettingCard').getBoundingClientRect();
            const header=document.querySelector('.topbar').getBoundingClientRect();
            window.scrollTo({top:window.scrollY+box.top-header.height-10,behavior:'instant'}); });
          await page.screenshot({ path:path.join(out,`MAYAP-Quạt-hút-mobile-${theme}.png`) });
        }
      }
      const controlLayout = await inspectControlLayout(page);
      assert.ok(controlLayout.toggles.length>=8, `${width}/${theme}: inspect both toggle sizes`);
      for (const toggle of controlLayout.toggles) {
        assert.ok(toggle.verticalError<=.5, `${width}/${theme}/${toggle.id}/${toggle.checked}: thumb must be centered`);
        assert.ok(toggle.edgeGap>=2 && toggle.edgeGap<=4, `${width}/${theme}/${toggle.id}/${toggle.checked}: equal end inset`);
        assert.ok(toggle.labelHeight>=44, `${toggle.id}: touch target must be at least 44px`);
        assert.equal(toggle.thumbBorder,0, `${toggle.id}: no dark thumb outline`);
      }
      assert.ok(controlLayout.gear.inside && controlLayout.gear.centerErrorX<=.01 && controlLayout.gear.centerErrorY<=.01, 'Settings gear must be centered inside its viewBox');
      if (width<800) assert.ok(controlLayout.gear.mobileCenterError<=.5,'Settings gear must be centered in mobile tab');
      assert.equal(controlLayout.hintIcons,0,'Settings help circles must be absent');
      assert.deepEqual(controlLayout.mismatchedBorders,[],'Standard button/input borders must match the shared line');
      if (width===390) {
        controls.push({theme,...controlLayout});
        const alarmCard = page.locator('#lightAlarmForm').locator('..').locator('..');
        await alarmCard.evaluate(el => { el.open=true; el.scrollIntoView({block:'center',behavior:'instant'}); });
        await alarmCard.screenshot({path:path.join(out,`MAYAP-controls-${theme}.png`)});
        await page.locator('.nav').screenshot({path:path.join(out,`MAYAP-nav-${theme}.png`)});
      }
      // Legacy config preserves thermal fan controls and disables unsupported stage profiles.
      await page.evaluate(() => { const h=window.__qa,d=h.state.devices[0]; const config={...d.config};
        h.VENT_PROFILE_KEYS.forEach(key=>delete config[key]); h.handleConfigReport(d,{config,revision:2,bootId:123}); });
      assert.equal(await page.locator('#ventProfileFields').evaluate(el => el.disabled), true);
      assert.equal(await page.locator('#ventSettingCard').isVisible(), true);
      assert.match(await page.locator('#ventSummary').textContent(), /ngưỡng nhiệt độ/);
      headers.push({width,height,theme,states:await checkHeaderStates(page,{width,height,theme})});
      assert.deepEqual(errors, []);
      results.push(`${width}x${height}/${theme}: three tabs, no overflow, centered controls, readable header and five states, fault popup accessible`);
      await context.close();
    }
    for (const theme of ['light','dark']) for (const scenario of ['unpaired','offline','humidifier']) {
      const operating=await setup(browser,{width:390,height:scenario==='humidifier' ? 932 : 850,mobile:true,theme,paired:scenario!=='unpaired'});
      await operating.page.evaluate(scenario=>{
        const h=window.__qa,d=h.state.devices[0];
        if (scenario==='offline') {d.presence.online=false;h.renderDevice();}
        if (scenario==='humidifier') h.handleConfigReport(d,{config:{...d.config,humidifierInstalled:true},revision:2,bootId:123});
      },scenario);
      const card=await operating.page.evaluate(()=>{
        const cells=[...document.querySelectorAll('.quickField,.outputStrip>div,.outputStrip>button')].map(el=>el.getBoundingClientRect()).filter(r=>r.height>0);
        const main=document.querySelector('.main'),form=document.getElementById('quickForm').getBoundingClientRect();
        return {count:cells.length,heights:cells.map(r=>r.height),scroll:main.scrollHeight-main.clientHeight,
          gap:document.querySelector('.nav').getBoundingClientRect().top-form.bottom};
      });
      assert.equal(card.count,scenario==='humidifier' ? 9 : 8,`${scenario}: optional hardware visibility preserved`);
      assert.ok(Math.max(...card.heights)-Math.min(...card.heights)<=.5,`${scenario}: equal cell heights`);
      assert.ok(card.scroll<=1 && card.gap>=8 && card.gap<=12,`${scenario}: full operating card fits close to navigation`);
      await operating.page.screenshot({path:path.join(out,`MAYAP-device-${scenario}-${theme}.png`),animations:'disabled'});
      layouts.push({theme,scenario,...card});
      await operating.context.close();
    }
    results.push('Operating layout: unpaired/offline eight cells and humidifier nine cells have equal heights; card fits within 8–12px of mobile navigation. Desktop batch cards end after the action row instead of stretching to the viewport bottom.');
    const { context, page } = await setup(browser, { width:390, mobile:true, theme:'light', dropFirst:true });
    const sessions = await page.evaluate(() => window.__transport.sessions);
    assert.ok(sessions.length >= 2);
    const retryMs = sessions[1].at-sessions[0].at;
    assert.ok(retryMs >= 650 && retryMs < 1200);
    assert.equal(await page.evaluate(() => window.__transport.subscriptions.length), 1);
    await swipe(page, 260, 35, -140);
    assert.equal(await page.evaluate(() => document.body.dataset.page), 'batch');
    await swipe(page, 260, 35, -140);
    assert.equal(await page.evaluate(() => document.body.dataset.page), 'settings');
    await swipe(page, 150, 35, 140);
    assert.equal(await page.evaluate(() => document.body.dataset.page), 'batch');
    // Actual touch move must paint before release; cancellation must return
    // the tab to its origin and leave controls available.
    const touch = await context.newCDPSession(page);
    const startDrag = async dx => {
      await touch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[{x:260,y:35}]});
      await touch.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[{x:260+dx,y:35}]});
      await page.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));
    };
    await startDrag(-45);
    const feedback=await page.locator('.page.active').evaluate(el=>new DOMMatrixReadOnly(getComputedStyle(el).transform).m41);
    assert.ok(feedback<0 && feedback>=-96,'Content follows the finger before release');
    await touch.send('Input.dispatchTouchEvent',{type:'touchCancel',touchPoints:[]});
    await page.waitForFunction(()=>!document.querySelector('.page[inert]') && !document.getAnimations().length);
    assert.equal(await page.locator('.page.active').evaluate(el=>el.style.transform),'');
    assert.equal(await page.evaluate(()=>document.body.dataset.page),'batch');
    await swipe(page, 260, 35, -30);
    assert.equal(await page.evaluate(()=>document.body.dataset.page),'batch','Short swipe snaps back');
    await page.evaluate(()=>{
      window.__swipeResponse={released:0,committed:0};
      document.addEventListener('pointerup',()=>{window.__swipeResponse.released=performance.now();},{once:true,capture:true});
      const observer=new MutationObserver(()=>{
        if (document.body.dataset.page==='settings') {window.__swipeResponse.committed=performance.now();observer.disconnect();}
      });
      observer.observe(document.body,{attributes:true,attributeFilter:['data-page']});
    });
    await startDrag(-140);
    await touch.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});
    assert.equal(await page.evaluate(()=>document.body.dataset.page),'settings','Valid swipe commits at release without an outgoing animation delay');
    const transition=await page.locator('.page.active').evaluate(el=>el.getAnimations().map(a=>a.effect.getTiming().duration));
    assert.ok(transition.every(duration=>duration<=100),'Swipe settles within 100ms');
    const response=await page.evaluate(()=>window.__swipeResponse);
    assert.ok(response.released>0 && response.committed>=response.released && response.committed-response.released<32,'Tab commitment responds within a frame budget at release');
    await page.waitForFunction(()=>!document.getAnimations().length);
    await touch.detach();
    await page.evaluate(()=>window.__qa.showPage('batch'));
    await swipe(page, 200, 300, 0, -120);
    assert.equal(await page.evaluate(() => document.body.dataset.page), 'batch');
    await page.locator('#batchName').fill('Mẻ đang sửa');
    await page.locator('#batchName').evaluate(el=>el.scrollIntoView({block:'center',behavior:'instant'}));
    const box = await page.locator('#batchName').boundingBox();
    assert.equal(await page.evaluate(({x,y})=>document.elementFromPoint(x,y)?.id,
      {x:box.x+box.width-20,y:box.y+box.height/2}),'batchName','Input gesture must start on the visible input');
    await swipe(page, box.x+box.width-20, box.y+box.height/2, -100);
    assert.equal(await page.evaluate(() => document.body.dataset.page), 'batch');
    await page.evaluate(() => { window.__qa.showPage('settings'); window.__qa.showPage('batch'); });
    assert.equal(await page.locator('#batchName').inputValue(), 'Mẻ đang sửa');
    await page.evaluate(() => document.getElementById('deviceDialog').showModal());
    await swipe(page, 260, 200, -140);
    assert.equal(await page.evaluate(() => document.body.dataset.page), 'batch');
    await page.evaluate(() => {
      document.getElementById('deviceDialog').close();
      window.__qa.showPage('settings');
      document.getElementById('lightAlarmForm').closest('details').open=true;
    });
    const alarmToggle = page.locator('#lightAfterBatchAlarmEnabled');
    const originalChecked = await alarmToggle.isChecked();
    await page.locator('#lightAfterBatchAlarmEnabled').locator('..').click();
    assert.equal(await alarmToggle.isChecked(),!originalChecked,'Native label click must toggle once');
    await alarmToggle.focus();
    await page.keyboard.press('Space');
    assert.equal(await alarmToggle.isChecked(),originalChecked,'Keyboard Space must still toggle');
    const hint = page.locator('#lightAlarmForm .settingHintTrigger').first();
    await hint.click();
    assert.equal(await hint.getAttribute('aria-expanded'),'true','Help remains reachable from text');
    const noteId = await hint.getAttribute('aria-controls');
    assert.equal(await page.locator('#'+noteId).isVisible(),true);
    await page.emulateMedia({reducedMotion:'reduce'});
    await page.evaluate(()=>window.__qa.showPage('batch'));
    await swipe(page, 150, 35, 140);
    assert.equal(await page.evaluate(()=>document.body.dataset.page),'device');
    assert.equal(await page.locator('.page.active').evaluate(el=>el.getAnimations().length),0,'Reduced motion changes tab without animation');
    results.push(`Touch swipe: release-to-commit ${(response.committed-response.released).toFixed(1)}ms in fixture, transition <=100ms, finger feedback, cancellation, short swipe, adjacent tabs, vertical scroll, input drag, dirty values, modal guards and reduced motion. Lost first sync retry ${Math.round(retryMs)}ms.`);
    results.push('Controls: label click and keyboard Space toggle once; help text still expands without an icon.');
    await context.close();
    const pairing = await setup(browser, { width:390, mobile:true, paired:false });
    let navigations = 0; pairing.page.on('framenavigated', frame => { if (frame===pairing.page.mainFrame()) navigations++; });
    await pairing.page.locator('#addDeviceBtn').click();
    await pairing.page.locator('#newDeviceId').fill('MAP-1234567890AB');
    await pairing.page.locator('#newDevicePin').fill('1234');
    await pairing.page.locator('#addDeviceForm button[type="submit"]').click();
    try { await pairing.page.waitForFunction(() => window.__qa.state.devices[0]?.snapshotAt); }
    catch (error) {
      console.error('Pairing diagnostics',await pairing.page.evaluate(()=>({
        transport:window.__transport,devices:window.__qa.state.devices.map(d=>({id:d.id,snapshotAt:d.snapshotAt})),
        form:document.getElementById('addDeviceForm').innerText,visible:document.getElementById('deviceDialog').open})),pairing.errors);
      throw error;
    }
    await pairing.page.waitForTimeout(650);
    assert.equal(navigations, 0, 'Pairing must not reload the document');
    assert.equal(await pairing.page.evaluate(() => window.__transport.connects), 1);
    assert.equal(await pairing.page.evaluate(() => window.__nativeStorageGet.call(localStorage, 'mayap.web.v10.mqtt.private')), null,
      'Credentials must not persist in native storage');
    assert.deepEqual(pairing.errors, []);
    results.push('Pairing: legacy PIN accepted, auth remains, no reload, one MQTT client, credentials absent from persistent storage.');
    await pairing.context.close();
    const retry = await setup(browser, { authFailures:1, width:390, mobile:true });
    assert.equal(await retry.page.evaluate(() => window.__transport.connects), 1);
    assert.deepEqual(retry.errors, []);
    results.push('Transient startup authentication 503: retries in background and connects without reload. Text/action contrast >=4.5 in both themes.');
    await retry.context.close();
    const system = await setup(browser, {scheme:'dark', width:390, mobile:true});
    assert.equal(await system.page.evaluate(() => getComputedStyle(document.documentElement).getPropertyValue('--surface').trim()), '#243944');
    assert.equal(await system.page.locator('meta[name="theme-color"]').getAttribute('content'),'#17272f');
    await system.page.emulateMedia({colorScheme:'light'});
    await system.page.waitForFunction(()=>document.querySelector('meta[name="theme-color"]').content==='#f3f8f9');
    assert.equal(await system.page.locator('meta[name="color-scheme"]').getAttribute('content'),'light');
    results.push('System theme updates palette, chart and browser theme color when OS preference changes.');
    await system.context.close();
    for (const theme of ['light','dark']) {
      const safe=await setup(browser,{width:390,height:844,mobile:true,theme});
      const emulation=await safe.context.newCDPSession(safe.page);
      for (const bottom of [34,0,34]) {
        await emulation.send('Emulation.setSafeAreaInsetsOverride',{insets:{top:24,topMax:24,bottom,bottomMax:34,left:0,leftMax:0,right:0,rightMax:0}});
        await safe.page.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));
        const layout=await safe.page.evaluate(()=>{
          const html=getComputedStyle(document.documentElement),body=getComputedStyle(document.body);
          const nav=document.querySelector('.nav').getBoundingClientRect(), sidebar=document.querySelector('.sidebar');
          const main=document.querySelector('.main'),bg=document.querySelector('meta[name="theme-color"]').content;
          const swatch=document.createElement('div');swatch.style.background=bg;document.body.append(swatch);
          const expected=getComputedStyle(swatch).backgroundColor;swatch.remove();
          return {navTop:nav.top,navBottom:nav.bottom,mainPadding:getComputedStyle(main).paddingBottom,
            sidebarColor:getComputedStyle(sidebar).backgroundColor,htmlColor:html.backgroundColor,bodyColor:body.backgroundColor,
            expected,scroll:main.scrollHeight-main.clientHeight,headerTop:document.querySelector('.headerReadings').getBoundingClientRect().top};
        });
        assert.equal(layout.htmlColor,layout.expected,'Root paints the system inset background');
        assert.equal(layout.bodyColor,layout.expected);
        assert.equal(layout.sidebarColor,layout.expected,'Bottom inset matches page background');
        assert.ok(layout.navBottom<=844-bottom-8+.5,'Navigation clears system controls');
        assert.ok(layout.headerTop>=24,'Readings clear status/notch inset');
        assert.ok(layout.scroll<=1,'Standard mobile viewport still fits with safe insets');
        if (bottom===34) await safe.page.screenshot({path:path.join(out,`MAYAP-safe-area-${theme}.png`)});
        safeAreas.push({theme,bottom,...layout});
      }
      assert.equal(new Set(safeAreas.filter(x=>x.theme===theme).map(x=>x.mainPadding)).size,1,'Changing browser chin does not reflow the content padding');
      await safe.page.evaluate(()=>window.__qa.showPage('batch'));
      assert.ok(await safe.page.locator('.main').evaluate(el=>el.scrollHeight-el.clientHeight)<=1,'Batch fits with safe areas');
      await emulation.detach();await safe.context.close();
    }
    const landscape=await setup(browser,{width:844,height:390,mobile:true,theme:'dark'});
    await swipe(landscape.page,600,35,-140);
    assert.equal(await landscape.page.evaluate(()=>document.body.dataset.page),'batch','Landscape touch supports tab switching');
    await landscape.page.locator('.main').evaluate(el=>{el.scrollTop=el.scrollHeight;});
    await landscape.page.evaluate(()=>window.__qa.showPage('device'));
    assert.equal(await landscape.page.locator('.main').evaluate(el=>el.scrollTop),0,'Tab change restores the start of the operating page');
    await landscape.context.close();
    results.push('Safe areas 24px top / 34px bottom: matching root/footer color, protected controls, stable padding when browser chin hides; landscape swipe and scroll restoration pass. Native OS bars require device verification.');
    fs.writeFileSync(path.join(out,'web-browser-qa.json'),JSON.stringify({ passed:true, results, palettes, controls, headers, layouts, safeAreas },null,2));
    console.log(results.join('\n'));
  } finally { await browser.close(); }
}
main().catch(error => { console.error(error); process.exitCode=1; });
