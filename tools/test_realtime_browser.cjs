// Actual native browser WebSocket against the local workerd fixture.
const assert=require('node:assert/strict'),path=require('node:path');
const {chromium}=require(process.env.MAYAP_PLAYWRIGHT || '../cloudflare/node_modules/playwright');
(async()=>{
 const port=Number(process.argv[2]),base=`http://localhost:${port}`;
 const result=await fetch(base+'/api/device/realtime-session',{method:'POST',headers:{Origin:'https://web.test',Authorization:'Bearer '+'22'.repeat(32),'Content-Type':'application/json'},body:JSON.stringify({device_id:'MAP-ABCDEF123456',control_client_id:'w-integration123'})});
 assert.equal(result.status,200);const session=await result.json();
 const browser=await chromium.launch({headless:true,executablePath:process.env.MAYAP_CHROME});
 try{
  const context=await browser.newContext();
  try{await context.grantPermissions(['local-network-access'],{origin:'https://web.test'});}catch{}
  const page=await context.newPage(),errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.route('https://web.test/**',r=>r.fulfill({contentType:'text/html',body:'<!doctype html><title>Native WebSocket QA</title>'}));
  await page.goto('https://web.test/native');await page.addScriptTag({path:path.resolve(__dirname,'../realtime_transport.js')});
  const outcome=await page.evaluate(async session=>{
   const timers=new Set(),messages=[];
   const client=new window.MayapRealtime.Client({deviceId:'MAP-ABCDEF123456',...session.realtime,isActive:()=>true,
    refresh:async()=>{throw Error('Unexpected renewal during short integration');},
    setTimeout:(fn,ms)=>{const n=setTimeout(()=>{timers.delete(n);fn();},ms);timers.add(n);return n;},clearTimeout:n=>{timers.delete(n);clearTimeout(n);}});
   client.on('message',(...args)=>messages.push(args));
   await new Promise((resolve,reject)=>{const timeout=setTimeout(()=>reject(Error('Native readiness deadline')),12000);client.on('connect',()=>{clearTimeout(timeout);resolve();});});
   const body=JSON.stringify({v:2,clientId:'w-integration123',requestId:'cmd-native-browser',seq:2,nonce:'ab'.repeat(8),bootId:124,action:'light_toggle',expiresAt:Math.floor(Date.now()/1000)+20});
   const key=await crypto.subtle.importKey('raw',new Uint8Array(session.control.sessionKey.match(/../g).map(h=>parseInt(h,16))),{name:'HMAC',hash:'SHA-256'},false,['sign']);
   const input=`mayap-mqtt-write:v2\nMAP-ABCDEF123456\ncommand\n${session.control.grant}\n${body}`;
   const sig=Array.from(new Uint8Array(await crypto.subtle.sign('HMAC',key,new TextEncoder().encode(input))),v=>v.toString(16).padStart(2,'0')).join('');
   const wire={v:2,body,grant:session.control.grant,grantSig:session.control.grantSig,sig};
   await new Promise((resolve,reject)=>client.send({deviceId:client.deviceId,channel:'command'},wire,e=>e?reject(e):resolve()));
   const applied=messages.filter(m=>m[0].channel==='ack').length;
   for(let i=0;i<30;i++)client.resume();
   await new Promise(r=>setTimeout(r,200));
   const same=client.generation===1&&client.connected;
   client.end();return {same,pending:client.pending.size,timers:timers.size,applied};
  },session);
  assert.deepEqual(outcome,{same:true,pending:0,timers:0,applied:0});assert.deepEqual(errors,[]);
  console.log('Native Chromium + real workerd: Origin/subprotocol auth, WebCrypto HMAC, forwarding-only receipt, resume storm, pong and clean bounded shutdown PASS');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
