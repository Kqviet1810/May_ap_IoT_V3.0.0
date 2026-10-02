const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

function worker(fetch, stored = {}) {
  const events = new Map(), timers = new Map(), values = new Map(Object.entries(stored));
  const cache = { match: async request => values.has(request.url) ? new Response(values.get(request.url)) : undefined,
    put: async (request, response) => values.set(request.url, await response.text()) };
  const opened = [];
  const ctx = { self:{ location:{origin:'https://web.test'}, addEventListener:(name,fn)=>events.set(name,fn) },
    caches:{open:async name=> { opened.push(name); return cache; }}, fetch, URL, Response,
    setTimeout:(fn,ms)=> { timers.set(1,{fn,ms}); return 1; }, clearTimeout:id=>timers.delete(id) };
  vm.runInNewContext(fs.readFileSync(require.resolve('../sw.js'),'utf8'),ctx);
  return { values, timers, opened,
    request(url) {
      let response; const lifetime=[];
      events.get('fetch')({request:new Request(url), respondWith:p=> {response=p;}, waitUntil:p=>lifetime.push(p)});
      return {get response(){return response;}, lifetime};
    }
  };
}

test('slow static fetch falls back promptly and still refreshes the versioned shell', async () => {
  let complete;
  const w = worker(()=>new Promise(resolve=> {complete=resolve;}), {'https://web.test/app.js':'cached'});
  const request = w.request('https://web.test/app.js');
  await new Promise(setImmediate);
  assert.equal(w.timers.get(1).ms,250);
  w.timers.get(1).fn();
  assert.equal(await (await request.response).text(),'cached');
  complete(new Response('fresh'));
  await Promise.all(request.lifetime);
  assert.equal(w.values.get('https://web.test/app.js'),'fresh');
  assert.ok(w.opened.every(name=>name==='mayap-web-v12.2.2'));
});

test('normal network remains fresh-first and cleans the fallback timer', async () => {
  const w = worker(async()=>new Response('fresh'), {'https://web.test/styles.css':'old'});
  const request = w.request('https://web.test/styles.css');
  assert.equal(await (await request.response).text(),'fresh');
  assert.equal(w.timers.size,0);
});

test('offline startup includes cached public config hardening; error pages do not replace code', async () => {
  const source = fs.readFileSync(require.resolve('../sw.js'),'utf8');
  assert.match(source, /'\.\/config\.js'/);
  const w = worker(async()=> {throw new Error('offline');}, {'https://web.test/config.js':'public security wrapper'});
  assert.equal(await (await w.request('https://web.test/config.js').response).text(),'public security wrapper');
  const failed = worker(async()=>new Response('server error',{status:503}), {'https://web.test/app.js':'good code'});
  assert.equal(await (await failed.request('https://web.test/app.js').response).text(),'good code');
  assert.equal(failed.values.get('https://web.test/app.js'),'good code');
});

test('Cloud auth requests never enter shell cache and pinned MQTT reuses the current release', async () => {
  let calls=0;
  const w = worker(async()=> {calls++; return new Response('network');}, {'https://web.test/vendor/mqtt.min.js':'pinned'});
  assert.equal(w.request('https://worker.test/api/device/mqtt-session').response,undefined);
  assert.equal(w.request('https://web.test/api/account/session').response,undefined);
  assert.equal(w.request('https://web.test/auth/google/callback?code=secret').response,undefined);
  assert.equal(w.request('https://worker.test/config.js').response,undefined);
  assert.equal(await (await w.request('https://web.test/vendor/mqtt.min.js').response).text(),'pinned');
  assert.equal(calls,0);
});
