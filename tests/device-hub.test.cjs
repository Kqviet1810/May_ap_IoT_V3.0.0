const test=require('node:test'),assert=require('node:assert/strict');
const {DatabaseSync}=require('node:sqlite'),fs=require('node:fs'),{pathToFileURL}=require('node:url');
const mod=Promise.all(['device-hub','realtime-auth','account-auth','account-worker'].map(n=>import(pathToFileURL(require('node:path').resolve('cloudflare/src/'+n+'.js')).href)));
const NativeResponse=global.Response;
global.Response=class extends NativeResponse{constructor(body,options){if(options?.status===101)return {status:101,headers:new Headers(options.headers),webSocket:options.webSocket};super(body,options);}};
global.WebSocketRequestResponsePair=class{constructor(request,response){this.request=request;this.response=response;}};
class Socket{
 constructor(){this.readyState=1;this.sent=[];this.data=null;}
 serializeAttachment(a){assert.ok(Buffer.byteLength(JSON.stringify(a))<=4096,'Application attachment budget (platform limit is 16KiB)');this.data=structuredClone(a);}
 deserializeAttachment(){return structuredClone(this.data);}
 send(s){if(this.readyState!==1)throw Error('CLOSED');assert.ok(Buffer.byteLength(s)<=2048);this.sent.push(JSON.parse(s));}
 close(code,reason){this.readyState=3;this.code=code;this.reason=reason;}
}
global.WebSocketPair=class{constructor(){this[0]=new Socket();this[1]=new Socket();}};
async function fixture(){
 const [hub,auth,account,worker]=await mod;const sql=new DatabaseSync(':memory:');sql.exec(fs.readFileSync('cloudflare/schema.sql','utf8'));
 for(const n of ['0003_telemetry_history','0004_accounts','0005_account_picture'])sql.exec(fs.readFileSync('cloudflare/migrations/'+n+'.sql','utf8'));
 let queries=0,gate=null;const DB={prepare(s){const st=sql.prepare(s);let args=[];return {bind(...v){args=v;return this;},async first(){queries++;const value=st.get(...args)||null;const pending=gate;gate=null;if(pending){pending.enter();await pending.promise;}return value;},async all(){queries++;return {results:st.all(...args)};},async run(){queries++;return {meta:st.run(...args)};}};}};
 DB.batch=async statements=>Promise.all(statements.map(st=>st.run()));
 const env={DB,MAYAP_SESSION_PEPPER:'test-session-pepper',DEVICE_KEY_PEPPER:'test-device-pepper',ALLOWED_ORIGIN:'https://web.test'};
 const session=await account.createSession(env,{sub:'owner-test',name:'Owner'}),id='MAP-1234567890AB',key='77'.repeat(32);
 sql.prepare('INSERT INTO devices(device_id,device_key_hash,created_at) VALUES(?,?,?)').run(id,await account.hash(key,{MAYAP_SESSION_PEPPER:env.DEVICE_KEY_PEPPER}),Date.now());
 sql.prepare("INSERT INTO user_devices(user_sub,device_id,role,created_at) VALUES(?,?,?,?)").run('owner-test',id,'owner',Date.now());
 const all=[],map=new Map();let alarm=null;
 const ctx={getWebSockets:()=>all,acceptWebSocket:s=>all.push(s),setWebSocketAutoResponse:p=>ctx.auto=p,
  blockConcurrencyWhile:()=>{throw Error('Global event gate must not be used');},
  storage:{sql:{exec(query,...args){const statement=sql.prepare(query);const rows=statement.columns().length ? statement.all(...args) : (statement.run(...args),[]);return {toArray:()=>rows};}},async get(k){return structuredClone(map.get(k));},async put(k,v){map.set(k,structuredClone(v));},async getAlarm(){return alarm;},async setAlarm(n){alarm=n;},async deleteAlarm(){alarm=null;},async transaction(fn){return fn(this);}}};
 let instance=new hub.DeviceHub(ctx,env);
 env.DEVICE_HUB={idFromName:n=>n,get:()=>({fetch:r=>instance.fetch(r)})};
 const claims={aud:'browser',deviceId:id,clientId:'w-browser123456',sessionId:session.id,userSub:'owner-test',role:'owner',sessionExpiresAt:session.expiry};
 async function browser(extra={}){const c={...claims,...extra},ticket=await auth.issueTicket(env,c);const verified=await auth.verifyTicket(env,ticket);
  const res=await instance.fetch(new Request('https://hub/connect',{headers:{Upgrade:'websocket','X-Mayap-Admission':JSON.stringify({...verified,kind:'browser'})}}));return {ws:all.at(-1),ticket,claims:c,res};}
 async function device(bootId=123){const res=await instance.fetch(new Request('https://hub/connect',{headers:{Upgrade:'websocket','X-Mayap-Admission':JSON.stringify({kind:'device',deviceId:id,bootId,keyHash:await account.hash(key,{MAYAP_SESSION_PEPPER:env.DEVICE_KEY_PEPPER})})}}));return {ws:all.at(-1),res};}
 async function command(seq=1,requestId='cmd-one',bootId=123,channel='command',overrides={}){
  const grant=await account.controlGrant(env,id,claims.clientId);const body=JSON.stringify({v:2,requestId,clientId:claims.clientId,bootId,seq,nonce:'aa'.repeat(8),expiresAt:Math.floor(Date.now()/1000)+20,action:'light_toggle',...overrides});
  const wire={v:2,grant:grant.grant,grantSig:grant.grantSig,body,sig:await account.hmacHex(Uint8Array.from(grant.sessionKey.match(/../g),x=>parseInt(x,16)),`mayap-mqtt-write:v2\n${id}\n${channel}\n${grant.grant}\n${body}`)};
  return {v:1,channel,payload:wire};
 }
 return {env,sql,account,auth,worker:worker.default,ctx,all,id,key,claims,session,browser,device,command,get hub(){return instance;},hibernate(){instance=new hub.DeviceHub(ctx,env);},get queries(){return queries;},
  message:(ws,msg)=>instance.webSocketMessage(ws,typeof msg==='string'?msg:JSON.stringify(msg)),clearQueries(){queries=0;},get alarm(){return alarm;},holdNextQuery(){let release,enter;const entered=new Promise(r=>enter=r);gate={enter,promise:new Promise(r=>release=r)};return {entered,release};}};
}
const events=(ws,c)=>ws.sent.filter(m=>m.channel===c),lastError=ws=>ws.sent.filter(m=>m.kind==='error').at(-1)?.code;
test('hibernation restores attachments and automatic ping response without telemetry D1',async()=>{
 const h=await fixture(),d=await h.device(),b=await h.browser();await h.message(b.ws,{v:1,channel:'session',payload:{clientId:h.claims.clientId,active:true,ttlMs:45000}});
 h.hibernate();h.clearQueries();await h.message(d.ws,{v:1,channel:'snapshot',payload:{bootId:123,runtime:{temperature:37.5}}});
 assert.equal(events(b.ws,'snapshot').length,1);assert.equal(h.queries,0);assert.equal(h.ctx.auto.response,'{"kind":"pong"}');
});
test('device reconnect replaces old generation and restores foreground viewer leases',async()=>{
 const h=await fixture(),d=await h.device(),b=await h.browser();await h.message(b.ws,{v:1,channel:'session',payload:{clientId:h.claims.clientId,active:true,ttlMs:45000,reminders:true,log:true}});
 const next=await h.device(124);assert.equal(d.ws.code,4001);assert.equal(events(next.ws,'session').length,1);assert.equal(events(next.ws,'session')[0].payload.reminders,true);assert.equal(events(next.ws,'session')[0].payload.log,true);
 await h.message(d.ws,{v:1,channel:'snapshot',payload:{bootId:123}});assert.equal(events(b.ws,'snapshot').length,0);
 await h.message(next.ws,{v:1,channel:'snapshot',payload:{bootId:123}});assert.equal(lastError(next.ws),'STALE_BOOT');
});
test('tickets are single-use across hibernation, bounded and tamper/expiry resistant',async()=>{
 const h=await fixture(),b=await h.browser();h.hibernate();const c=await h.auth.verifyTicket(h.env,b.ticket);
 const replay=await h.hub.fetch(new Request('https://hub/connect',{headers:{Upgrade:'websocket','X-Mayap-Admission':JSON.stringify({...c,kind:'browser'})}}));assert.equal(replay.status,403);
 assert.equal(await h.auth.verifyTicket(h.env,b.ticket+'x'),null);assert.equal(await h.auth.verifyTicket(h.env,b.ticket,Date.now()+61000),null);
 assert.equal(h.sql.prepare('SELECT COUNT(*) AS n FROM realtime_tickets').get().n,1);
});
test('eight browser cap, duplicate client replacement and bounded 4KiB attachments',async()=>{
 const h=await fixture();for(let i=0;i<8;i++)assert.equal((await h.browser({clientId:'w-browser00000'+i})).res.status,101);
 assert.equal((await h.browser({clientId:'w-browser-overflow'})).res.status,429);
 assert.equal((await h.browser({clientId:'w-browser000000'})).res.status,101);
 assert.equal(h.hub.sockets('browser').length,8);
});
test('exact signed retry is forwarded; changed signature/id, sequence and old boot are rejected',async()=>{
 const h=await fixture(),d=await h.device(),b=await h.browser(),cmd=await h.command();
 await h.message(b.ws,cmd);await h.message(b.ws,cmd);assert.equal(events(d.ws,'command').length,2);assert.equal(events(b.ws,'ack').length,0);
 await h.message(b.ws,await h.command(1,'different'));assert.equal(lastError(b.ws),'REPLAY');
 await h.message(b.ws,await h.command(2,'cmd-one'));assert.equal(lastError(b.ws),'REPLAY');
 await h.message(b.ws,await h.command(3,'old-boot',122));assert.equal(lastError(b.ws),'INVALID_SIGNATURE_OR_EXPIRY');
 cmd.payload.sig='0'.repeat(64);await h.message(b.ws,cmd);assert.equal(lastError(b.ws),'INVALID_SIGNATURE_OR_EXPIRY');
 assert.equal(events(d.ws,'command').length,2);
});
test('parallel writes serialize anti-replay state across asynchronous D1/crypto awaits',async()=>{
 const h=await fixture(),d=await h.device(),b=await h.browser();const [one,two]=await Promise.all([h.command(5,'one'),h.command(4,'two')]);
 await Promise.all([h.message(b.ws,one),h.message(b.ws,two)]);assert.equal(events(d.ws,'command').length,1);assert.equal(lastError(b.ws),'REPLAY');
});
test('viewer and revoked ownership/session cannot forward writes or renew reads',async()=>{
 for(const mutation of ["UPDATE user_devices SET role='viewer'",'DELETE FROM user_devices','UPDATE user_sessions SET revoked_at=1','UPDATE users SET disabled=1']){
  const h=await fixture(),d=await h.device(),b=await h.browser(),cmd=await h.command();h.sql.exec(mutation);await h.message(b.ws,cmd);
  assert.equal(lastError(b.ws),'ACCESS_DENIED');assert.equal(events(d.ws,'command').length,0);
  const ticket=await h.auth.issueTicket(h.env,h.claims);await h.message(b.ws,{kind:'renew',ticket});assert.equal(lastError(b.ws),'ACCESS_DENIED');
 }
});
test('hidden/browser-idle stops telemetry but signed terminal ACK remains deliverable',async()=>{
 const h=await fixture(),d=await h.device(),b=await h.browser();await h.message(b.ws,{v:1,channel:'session',payload:{clientId:h.claims.clientId,active:true,foreground:false,ttlMs:45000}});
 assert.equal(events(d.ws,'session').at(-1).payload.active,false);
 await h.message(d.ws,{v:1,channel:'snapshot',payload:{bootId:123}});assert.equal(events(b.ws,'snapshot').length,0);
 await h.message(d.ws,{v:1,channel:'ack',payload:{bootId:123,requestId:'cmd-one',sig:'device'}});assert.equal(events(b.ws,'ack').length,1);
});
test('slow consumers are closed after at most 16 outstanding events; credit cannot forge receipt',async()=>{
 const h=await fixture(),d=await h.device(),b=await h.browser();await h.message(b.ws,{v:1,channel:'session',payload:{clientId:h.claims.clientId,active:true,ttlMs:45000}});
 for(let i=0;i<20;i++)await h.message(d.ws,{v:1,channel:'snapshot',payload:{bootId:123,i}});
 assert.equal(b.ws.code,1013);assert.ok(events(b.ws,'snapshot').length<=16);
 const next=await h.browser();await h.message(next.ws,{kind:'received',deliveryId:999});assert.equal(next.ws.data.received,0);
 const n=next.ws.data.delivery;await h.message(next.ws,{kind:'received',deliveryId:n});assert.equal(next.ws.data.received,n);
});
test('alarm expires stale device and read leases with no repeating idle timers',async()=>{
 const h=await fixture(),d=await h.device(),b=await h.browser();b.ws.data.until=Date.now()-1;d.ws.data.lastAt=Date.now()-210001;
 await h.hub.alarm();assert.equal(b.ws.code,4003);assert.equal(d.ws.code,4002);assert.equal(h.alarm,null);
});
test('device offline never fabricates controller ACK; invalid/oversized binary frames are bounded',async()=>{
 const h=await fixture(),b=await h.browser();await h.message(b.ws,await h.command());assert.equal(lastError(b.ws),'DEVICE_OFFLINE');assert.equal(events(b.ws,'ack').length,0);
 await h.message(b.ws,'x'.repeat(2049));assert.equal(b.ws.code,1009);
 const next=await h.browser();await h.message(next.ws,'{');assert.equal(next.ws.code,1007);
});
test('outer Worker validates exact origin, device credential, boot and scoped URL before Hub binding',async()=>{
 const h=await fixture();const request=(path,headers)=>h.worker.fetch(new Request('https://api.test'+path,{headers:{Upgrade:'websocket',...headers}}),h.env,{});
 assert.equal((await request('/realtime/device/'+h.id,{})).status,401);
 assert.equal((await request('/realtime/device/'+h.id,{Authorization:'Bearer '+h.key,'X-Mayap-Boot':'123',Origin:'https://web.test'})).status,403);
 assert.equal((await request('/realtime/device/'+h.id,{Authorization:'Bearer '+h.key,'X-Mayap-Boot':'0'})).status,401);
 assert.equal((await request('/realtime/device/'+h.id,{Authorization:'Bearer '+h.key,'X-Mayap-Boot':'123'})).status,101);
 const ticket=await h.auth.issueTicket(h.env,h.claims),headers={Origin:'https://evil.test','Sec-WebSocket-Protocol':'mayap.v1, ticket.'+ticket};
 assert.equal((await request('/realtime/browser/'+h.id,headers)).status,403);
 headers.Origin='https://web.test';assert.equal((await request('/realtime/browser/MAP-000000000000',headers)).status,403);
 assert.equal((await request('/realtime/browser/'+h.id,headers)).status,101);
});
test('a day of admitted telemetry has no D1 hot path and stable attachment/credit bounds',async()=>{
 const h=await fixture(),d=await h.device(),b=await h.browser();await h.message(b.ws,{v:1,channel:'session',payload:{clientId:h.claims.clientId,active:true,ttlMs:60000}});h.clearQueries();
 for(let i=0;i<86400;i++){
  if(i%3000===0){h.hibernate();b.ws.data.rateAt=Date.now()-10001;b.ws.data.until=Date.now()+300000;b.ws.data.watchUntil=Date.now()+60000;}
  b.ws.data.rateAt=Date.now()-10001;d.ws.data.rateAt=Date.now()-10001;
  await h.message(d.ws,{v:1,channel:'snapshot',payload:{bootId:123,i,runtime:{temperature:37.5}}});
  await h.message(b.ws,{kind:'received',deliveryId:b.ws.data.delivery});
  // Test sink drains just like an actual browser, avoiding a test-only queue.
  d.ws.sent.length=0;b.ws.sent.length=0;
 }
 assert.equal(h.queries,0);assert.equal(h.hub.sockets('browser').length,1);assert.equal(b.ws.data.delivery-b.ws.data.received,0);
});

test('rotated device credential cannot receive future control and internal invalidation closes it',async()=>{
 const h=await fixture(),d=await h.device(),b=await h.browser();h.sql.exec("UPDATE devices SET device_key_hash='rotated'");
 await h.message(b.ws,await h.command());assert.equal(lastError(b.ws),'DEVICE_REAUTH_REQUIRED');assert.equal(events(d.ws,'command').length,0);assert.equal(d.ws.code,4003);
 const next=await h.device();await h.hub.fetch(new Request('https://hub/invalidate-device',{method:'POST'}));assert.equal(next.ws.code,4003);
});
test('expired signed command, wrong client and tampered body cannot cross the Hub',async()=>{
 const h=await fixture(),d=await h.device(),b=await h.browser();
 for(const override of [{expiresAt:Math.floor(Date.now()/1000)-1},{clientId:'w-wrong-client'},{nonce:'bad'}])await h.message(b.ws,await h.command(1,'bad',123,'command',override));
 assert.equal(events(d.ws,'command').length,0);assert.equal(lastError(b.ws),'INVALID_SIGNATURE_OR_EXPIRY');
});

test('maximum-length signed transaction cache remains bounded and exact retries survive 16 entries',async()=>{
 const h=await fixture(),d=await h.device(),b=await h.browser();let first;
 for(let i=1;i<=16;i++){const msg=await h.command(i,('r'+i+'x'.repeat(38)).slice(0,39));first ||= msg;await h.message(b.ws,msg);}
 assert.equal(b.ws.data.requests.length,16);assert.ok(Buffer.byteLength(JSON.stringify(b.ws.data))<4096);
 await h.message(b.ws,first);assert.equal(events(d.ws,'command').length,17);
});

test('a stalled browser D1 validation does not block device telemetry or another viewer',async()=>{
 const h=await fixture(),d=await h.device(),b=await h.browser(),other=await h.browser({clientId:'w-independent123'});
 await h.message(other.ws,{v:1,channel:'session',payload:{clientId:'w-independent123',active:true,ttlMs:45000}});
 const cmd=await h.command(),gate=h.holdNextQuery(),write=h.message(b.ws,cmd);await gate.entered;
 await h.message(d.ws,{v:1,channel:'snapshot',payload:{bootId:123,temperature:37.5}});
 assert.equal(events(other.ws,'snapshot').length,1);assert.equal(events(d.ws,'command').length,0);
 gate.release();await write;assert.equal(events(d.ws,'command').length,1);
});
test('revocation fences validation already in flight and closes reads immediately',async()=>{
 const h=await fixture(),d=await h.device(),b=await h.browser(),cmd=await h.command();
 const gate=h.holdNextQuery(),write=h.message(b.ws,cmd);await gate.entered;
 h.sql.exec('DELETE FROM user_devices');
 const response=await h.hub.fetch(new Request('https://hub/invalidate-browser',{method:'POST',body:JSON.stringify({userSub:h.claims.userSub})}));
 assert.equal(response.status,200);assert.equal(b.ws.reason,'ACCESS_REVOKED');
 const count=b.ws.sent.length;await h.message(d.ws,{v:1,channel:'snapshot',payload:{bootId:123}});assert.equal(b.ws.sent.length,count);
 gate.release();await write;assert.equal(events(d.ws,'command').length,0);
});
test('admission already awaiting D1 cannot survive a revocation fence',async()=>{
 const h=await fixture(),gate=h.holdNextQuery(),admission=h.browser();await gate.entered;
 h.sql.exec('DELETE FROM user_devices');await h.hub.fetch(new Request('https://hub/invalidate-browser',{method:'POST',body:JSON.stringify({userSub:h.claims.userSub})}));
 gate.release();assert.equal((await admission).res.status,403);assert.equal(h.hub.sockets('browser').length,0);
});
test('write validated against an old device cannot reach its replacement',async()=>{
 const h=await fixture(),old=await h.device(),b=await h.browser(),cmd=await h.command(),gate=h.holdNextQuery(),write=h.message(b.ws,cmd);await gate.entered;
 const next=await h.device(124);gate.release();await write;
 assert.equal(events(old.ws,'command').length,0);assert.equal(events(next.ws,'command').length,0);
});
test('per-browser async queue is bounded while the device remains available',async()=>{
 const h=await fixture(),d=await h.device(),b=await h.browser(),cmd=await h.command(),gate=h.holdNextQuery(),write=h.message(b.ws,cmd);await gate.entered;
 const queued=Array.from({length:12},()=>h.message(b.ws,cmd));
 await h.message(d.ws,{v:1,channel:'snapshot',payload:{bootId:123}});
 assert.equal(d.ws.readyState,1);assert.equal(b.ws.code,1013);gate.release();await Promise.all([write,...queued]);
 assert.equal(h.hub.queues.size,0);assert.equal(events(d.ws,'command').length,0);
});
test('persistent exact-retry and seq fences survive socket replacement and hibernation',async()=>{
 const h=await fixture(),d=await h.device(),b=await h.browser(),cmd=await h.command(5,'stable');await h.message(b.ws,cmd);
 const next=await h.browser();h.hibernate();await h.message(next.ws,cmd);assert.equal(events(d.ws,'command').length,2);
 await h.message(next.ws,await h.command(6,'stable'));assert.equal(lastError(next.ws),'REPLAY');
 await h.message(next.ws,await h.command(4,'older'));assert.equal(lastError(next.ws),'REPLAY');assert.equal(events(d.ws,'command').length,2);
 h.sql.exec('UPDATE realtime_replay SET expiry=1');await h.hub.alarm();assert.equal(h.sql.prepare('SELECT COUNT(*) AS n FROM realtime_replay').get().n,0);
});
test('more than 64 still-live one-use tickets do not poison admission; storm rate is bounded',async()=>{
 const h=await fixture(),real=Date.now;let now=real();Date.now=()=>now;
 try {
  for(let i=0;i<96;i++) {if(i&&i%32===0)now+=10000;assert.equal(h.hub.consumeTicket({...h.claims,nonce:i.toString(16).padStart(32,'0'),exp:Math.floor(now/1000)+60}),true);}
  assert.equal(h.sql.prepare('SELECT COUNT(*) AS n FROM realtime_tickets').get().n,96);
  assert.equal(h.hub.consumeTicket({...h.claims,nonce:'0'.repeat(32),exp:Math.floor(now/1000)+60}),false);
  for(let i=96;i<104;i++)assert.equal(h.hub.consumeTicket({...h.claims,nonce:i.toString(16).padStart(32,'0'),exp:Math.floor(now/1000)+60}),true);
  assert.equal(h.hub.consumeTicket({...h.claims,nonce:'f'.repeat(32),exp:Math.floor(now/1000)+60}),false);
 }finally{Date.now=real;}
});
test('device ingress bounds quota abuse without D1 access',async()=>{
 const h=await fixture(),d=await h.device();h.clearQueries();
 for(let i=0;i<161;i++)await h.message(d.ws,{v:1,channel:'snapshot',payload:{bootId:123,i}});
 assert.equal(d.ws.reason,'DEVICE_RATE_LIMIT');assert.equal(h.queries,0);
});
test('owner access revocation and session logout close sockets before successful API response',async()=>{
 const h=await fixture();await h.device();
 const viewer=await h.account.createSession(h.env,{sub:'viewer-test',name:'Viewer'});
 h.sql.prepare("INSERT INTO user_devices VALUES(?,?,'viewer',?)").run('viewer-test',h.id,Date.now());
 const v=await h.browser({userSub:'viewer-test',sessionId:viewer.id,sessionExpiresAt:viewer.expiry,role:'viewer',clientId:'w-viewer-test123'});
 const call=(path,body,token=h.session.token)=>h.worker.fetch(new Request('https://web.test'+path,{method:'POST',headers:{Origin:'https://web.test',Authorization:'Bearer '+token,'Content-Type':'application/json'},body:JSON.stringify(body)}),h.env,{});
 assert.equal((await call('/api/device/revoke-access',{device_id:h.id,user_sub:h.claims.userSub},viewer.token)).status,403);
 assert.equal((await call('/api/device/revoke-access',{device_id:h.id,user_sub:'viewer-test'})).status,200);assert.equal(v.ws.reason,'ACCESS_REVOKED');
 assert.equal((await call('/api/device/revoke-access',{device_id:h.id,user_sub:'viewer-test'})).status,200);
 const own=await h.browser();assert.equal((await call('/api/account/logout',{})).status,200);assert.equal(own.ws.reason,'ACCESS_REVOKED');
});

test('receive credits and visibility leases bypass a stalled write FIFO',async()=>{
 const h=await fixture(),d=await h.device(),b=await h.browser();
 await h.message(b.ws,{v:1,channel:'session',payload:{clientId:h.claims.clientId,active:true,ttlMs:60000}});
 const cmd=await h.command(),gate=h.holdNextQuery(),write=h.message(b.ws,cmd);await gate.entered;
 for(let i=0;i<12;i++){await h.message(d.ws,{v:1,channel:'snapshot',payload:{bootId:123,i}});await h.message(b.ws,{kind:'received',deliveryId:b.ws.data.delivery});}
 await h.message(b.ws,{v:1,channel:'session',payload:{clientId:h.claims.clientId,active:true,foreground:false,ttlMs:60000}});
 assert.equal(b.ws.readyState,1);assert.equal(b.ws.data.received,b.ws.data.delivery);assert.equal(events(d.ws,'session').at(-1).payload.active,false);
 gate.release();await write;assert.equal(events(d.ws,'command').length,1);
});

test('Hub-wide async cap includes old sockets whose external validation never settles',async()=>{
 const h=await fixture(),d=await h.device();let release;const blocked=new Promise(r=>release=r);
 const pending=Array.from({length:80},()=>h.hub.serial({},()=>blocked));
 await assert.rejects(h.hub.serial({},()=>Promise.resolve()),/BUSY/);assert.equal(h.hub.queues.size,80);
 await h.message(d.ws,{v:1,channel:'snapshot',payload:{bootId:123}});assert.equal(d.ws.readyState,1);
 release();await Promise.all(pending);await Promise.resolve();assert.equal(h.hub.queues.size,0);assert.equal(h.hub.queued,0);
});
test('device freshness is rechecked after a slow external write validation',async()=>{
 const h=await fixture(),d=await h.device(),b=await h.browser(),cmd=await h.command(1,'slow-config',123,'config/set'),gate=h.holdNextQuery(),write=h.message(b.ws,cmd);await gate.entered;
 const real=Date.now;Date.now=()=>real()+211000;
 try{gate.release();await write;assert.equal(events(d.ws,'config/set').length,0);assert.equal(lastError(b.ws),'CONNECTION_CHANGED');}
 finally{Date.now=real;}
});
