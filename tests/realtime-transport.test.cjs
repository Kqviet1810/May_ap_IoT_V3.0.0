const test=require('node:test'), assert=require('node:assert/strict');
const fs=require('node:fs'), vm=require('node:vm');
const {Client,CAP,MAX_PENDING}=require('../realtime_transport.js');
const flush=()=>new Promise(setImmediate);
function fixture(ClientType=Client,browserRoot=null,injectTimers=true){
 let now=0,id=0,active=true,refreshes=0;const timers=new Map(),sockets=[],errors=[],packets=[],events=[];
 class Socket {
  constructor(url,protocols){this.url=url;this.protocols=protocols;this.readyState=1;this.bufferedAmount=0;this.handlers={};this.sent=[];sockets.push(this);}
  addEventListener(k,f){this.handlers[k]=f;} send(s){this.sent.push(s);} close(){this.readyState=3;this.handlers.close?.();}
  message(v){this.handlers.message?.({data:typeof v==='string'?v:JSON.stringify(v)});}
 }
 const timerOptions={setTimeout:(f,ms)=>{const n=++id;timers.set(n,{f,at:now+ms,ms});return n;},clearTimeout:n=>timers.delete(n)};
 if(browserRoot)for(const name of ['setTimeout','clearTimeout'])browserRoot[name]=function(...args){
  if(this!==browserRoot)throw new TypeError('Illegal invocation');
  browserRoot.timerCalls.push(name);return timerOptions[name](...args);
 };
 const client=new ClientType({deviceId:'MAP-1234567890AB',url:'wss://hub.test/realtime/browser/MAP-1234567890AB',ticket:'initial',WebSocket:Socket,
  clock:()=>now,random:()=>0,isActive:()=>active,refresh:async()=>{refreshes++;return {url:'wss://hub.test/realtime/browser/MAP-1234567890AB',ticket:'fresh-'+refreshes};},
  ...(injectTimers?timerOptions:{})});
 client.on('error',e=>errors.push(e));client.on('message',(...a)=>packets.push(a));client.on('close',()=>events.push('close'));
 return {client,sockets,timers,errors,packets,events,get refreshes(){return refreshes;},setActive:v=>active=v,
  ready(){sockets.at(-1).message({kind:'ready',deviceId:client.deviceId});},
  async advance(ms){now+=ms;for(const [n,t]of [...timers])if(timers.has(n)&&t.at<=now){timers.delete(n);await t.f();}await flush();}};
}
const route={deviceId:'MAP-1234567890AB',channel:'command'};
const wire=id=>({body:JSON.stringify({requestId:id}),sig:'test'});
function browserRoot(){
 const root={URL,TextEncoder,queueMicrotask,timerCalls:[]};root.window=root;
 const context=vm.createContext(root);
 vm.runInContext(fs.readFileSync(require.resolve('../realtime_transport.js'),'utf8'),context,{filename:'realtime_transport.js'});
 return vm.runInContext('window',context);
}
test('Safari-style native timers retain Window receiver through connect, close and resume',async()=>{
 const root=browserRoot(),h=fixture(root.MayapRealtime.Client,root,false);
 // These browser APIs reject a Client (or any other object) as their receiver.
 assert.throws(()=>Reflect.apply(root.setTimeout,h.client,[()=>{},1]),/Illegal invocation/);
 assert.throws(()=>Reflect.apply(root.clearTimeout,h.client,[0]),/Illegal invocation/);
 await flush();assert.equal(h.errors.length,0);assert.equal(h.timers.size,1);h.ready();
 const outcomes=[];h.client.send(route,wire('native-timer'),e=>outcomes.push(e.code));
 h.sockets[0].close();assert.deepEqual(outcomes,['UNCERTAIN']);assert.equal(h.timers.size,1);
 h.client.resume();await flush();assert.equal(h.refreshes,1);assert.equal(h.sockets.length,2);h.ready();
 h.client.resume();h.sockets[1].message({kind:'pong'});await h.advance(8000);
 assert.equal(h.client.connected,true);assert.equal(h.errors.length,0);
 h.client.end();assert.equal(h.timers.size,0);
 assert.ok(root.timerCalls.includes('setTimeout'));assert.ok(root.timerCalls.includes('clearTimeout'));
});
test('injected timers override browser native timers without rebinding',async()=>{
 const root=browserRoot(),h=fixture(root.MayapRealtime.Client,root);
 assert.equal(h.client.setTimer,h.client.options.setTimeout);assert.equal(h.client.clearTimer,h.client.options.clearTimeout);
 await flush();h.ready();h.sockets[0].close();await h.advance(1000);h.ready();h.client.resume();
 h.sockets[1].message({kind:'pong'});h.client.end();
 assert.equal(h.errors.length,0);assert.equal(h.refreshes,1);assert.equal(h.timers.size,0);assert.deepEqual(root.timerCalls,[]);
});
test('ticket stays in subprotocol; readiness scopes the socket to one selected device',async()=>{
 const h=fixture();await flush();assert.deepEqual(h.sockets[0].protocols,['mayap.v1','ticket.initial']);assert.equal(new URL(h.sockets[0].url).search,'');
 assert.equal(h.client.connected,false);h.ready();assert.equal(h.client.connected,true);
 assert.throws(()=>h.client.send({...route,deviceId:'MAP-000000000000'},wire('x')),/TRANSPORT_ERROR/);h.client.end();
});
test('forward receipt is separate from controller ACK and credit acknowledges bounded receives',async()=>{
 const h=fixture();await flush();h.ready();let receipt=0;h.client.send(route,wire('one'),e=>{assert.equal(e,null);receipt++;});
 h.sockets[0].message({kind:'forwarded',requestId:'one'});assert.equal(receipt,1);assert.equal(h.packets.length,0);
 h.sockets[0].message({v:1,channel:'ack',payload:{requestId:'one',sig:'device'},deliveryId:2});
 assert.equal(h.packets[0][1].sig,'device');assert.deepEqual(JSON.parse(h.sockets[0].sent.at(-1)),{kind:'received',deliveryId:2});h.client.end();
});
test('pending writes, frames and browser TCP buffers have fixed bounds',async()=>{
 const h=fixture();await flush();h.ready();for(let i=0;i<MAX_PENDING;i++)h.client.send(route,wire('r'+i),()=>{});
 assert.throws(()=>h.client.send(route,wire('extra'),()=>{}),/PENDING_LIMIT/);assert.throws(()=>h.client.send(route,wire('r0'),()=>{}),/PENDING_LIMIT/);
 assert.throws(()=>h.client.send(route,{body:'x'.repeat(CAP)}),/FRAME_OR_BUFFER_LIMIT/);
 h.sockets[0].bufferedAmount=CAP*4+1;assert.throws(()=>h.client.send(route,{}),/FRAME_OR_BUFFER_LIMIT/);h.client.end();assert.equal(h.timers.size,0);
});
test('missing receipt and network close yield UNCERTAIN exactly once',async()=>{
 const h=fixture();await flush();h.ready();const outcomes=[];
 h.client.send(route,wire('one'),e=>outcomes.push(e.code));await h.advance(8000);assert.deepEqual(outcomes,['UNCERTAIN']);
 h.sockets[0].message({kind:'forwarded',requestId:'one'});assert.equal(outcomes.length,1);
 h.client.send(route,wire('two'),e=>outcomes.push(e.code));h.sockets[0].close();assert.deepEqual(outcomes,['UNCERTAIN','UNCERTAIN']);h.client.end();assert.equal(h.timers.size,0);
});
test('reconnect refreshes single-use ticket and ignores old socket messages',async()=>{
 const h=fixture();await flush();h.ready();const old=h.sockets[0];old.close();assert.equal(h.timers.size,1);
 await h.advance(1000);assert.equal(h.refreshes,1);assert.equal(h.sockets.length,2);h.ready();
 old.message({v:1,channel:'snapshot',payload:{bad:true}});assert.equal(h.packets.length,0);
 assert.equal(h.sockets[1].protocols[1],'ticket.fresh-1');h.client.end();
});
test('readiness deadline and malformed or oversized inbound frames reconnect safely',async()=>{
 for(const frame of [null,'{', 'x'.repeat(CAP+1), {kind:'ready',deviceId:'MAP-000000000000'}]){
  const h=fixture();await flush();if(frame===null)await h.advance(15000);else h.sockets[0].message(frame);
  assert.equal(h.client.connected,false);assert.ok(h.timers.size<=1);h.client.end();assert.equal(h.timers.size,0);
 }
});
test('resume storms coalesce a single probe; pong preserves socket even with device offline',async()=>{
 const h=fixture();await flush();h.ready();for(let i=0;i<30;i++)h.client.resume();
 assert.equal(h.sockets[0].sent.length,1);h.sockets[0].message({kind:'pong'});await h.advance(8000);assert.equal(h.sockets.length,1);assert.equal(h.client.connected,true);h.client.end();
});
test('dead socket closes after bounded probe and reconnects once',async()=>{
 const h=fixture();await flush();h.ready();h.client.resume();await h.advance(8000);assert.equal(h.events.length,1);
 await h.advance(1000);assert.equal(h.sockets.length,2);assert.equal(h.refreshes,1);h.client.end();
});
test('OS suspension re-probes rather than declaring a socket dead on a late timer',async()=>{
 const h=fixture();await flush();h.ready();h.client.probe();await h.advance(120000);assert.equal(h.sockets.length,1);assert.equal(h.client.connected,true);
 h.sockets[0].message({kind:'pong'});await h.advance(8000);assert.equal(h.sockets.length,1);h.client.end();
});
test('inactive browser does not renew or reconnect; visible resume restores one owner',async()=>{
 const h=fixture();await flush();h.ready();h.setActive(false);await h.advance(250000);assert.equal(h.refreshes,0);
 h.sockets[0].close();assert.equal(h.timers.size,0);h.setActive(true);h.client.resume();await flush();assert.equal(h.refreshes,1);assert.equal(h.sockets.length,2);h.client.end();
});
test('connection lease renews outside command path and rejected renewal reconnects',async()=>{
 const h=fixture();await flush();h.ready();for(let i=0;i<8;i++){h.sockets[0].message({kind:'pong'});await h.advance(30000);}
 assert.equal(h.refreshes,1);assert.equal(JSON.parse(h.sockets[0].sent.at(-1)).kind,'renew');
 h.sockets[0].message({kind:'renewed'});assert.equal(h.client.refreshing,false);
 h.client.renew('new');await h.advance(30000);assert.equal(h.client.connected,false);h.client.end();
});
test('seven simulated days of traffic/reconnect retain bounded timers and pending memory',async()=>{
 const h=fixture();await flush();h.ready();let maxTimers=0;
 for(let i=0;i<7*24*120;i++){
  if(i%97===0){h.sockets.at(-1).close();await h.advance(1000);h.ready();}
  h.sockets.at(-1).message({kind:'pong'});await h.advance(30000);maxTimers=Math.max(maxTimers,h.timers.size);
  assert.equal(h.client.pending.size,0);assert.ok(h.timers.size<=2);
 }
 assert.ok(maxTimers<=2);h.client.end();assert.equal(h.timers.size,0);
});

test('renewal timeout clears its state so a fresh socket remains healthy',async()=>{
 const h=fixture();await flush();h.ready();h.client.renew('one-use-renewal');await h.advance(30000);assert.equal(h.client.connected,false);
 await h.advance(1000);h.ready();h.sockets.at(-1).message({kind:'pong'});await h.advance(30000);
 assert.equal(h.client.connected,true);assert.equal(h.client.refreshing,false);h.client.end();
});
test('same ticket is renewed once and reconnect/resume storms share one open attempt',async()=>{
 const h=fixture();await flush();h.ready();h.client.renew('new-ticket');h.client.renew('new-ticket');
 assert.equal(h.sockets[0].sent.filter(t=>JSON.parse(t).kind==='renew').length,1);
 h.sockets[0].close();for(let i=0;i<30;i++)h.client.resume();await flush();
 assert.equal(h.refreshes,1);assert.equal(h.sockets.length,2);h.client.end();
});
test('every correlated Hub rejection preserves its code and stage rather than fake UNCERTAIN',async()=>{
 const h=fixture();await flush();h.ready();
 for(const code of ['ACCESS_DENIED','DEVICE_OFFLINE','INVALID_SIGNATURE_OR_EXPIRY','CONNECTION_CHANGED','DEVICE_REAUTH_REQUIRED','DEVICE_SEND_FAILED','REPLAY','BUSY']){
  let outcome;h.client.send(route,wire(code),e=>outcome=e);h.sockets[0].message({kind:'error',code,requestId:code});
  assert.equal(outcome.code,code);assert.equal(outcome.hubCode,code);assert.equal(outcome.stage,'HUB_REJECTED');
  assert.equal(h.client.pending.size,0);
 }
 h.client.end();
});
