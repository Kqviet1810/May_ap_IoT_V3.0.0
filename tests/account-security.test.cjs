const test=require('node:test'), assert=require('node:assert/strict'), fs=require('node:fs');
const { DatabaseSync }=require('node:sqlite');
const { pathToFileURL }=require('node:url');
const base='https://worker.example',frontend='https://kqviet1810.github.io';
const modules=Promise.all([import('../cloudflare/src/account-worker.js'),import('../cloudflare/src/account-auth.js'),
  import(pathToFileURL(require.resolve('../cloudflare/node_modules/jose')).href)]);
function database(){
  const sql=new DatabaseSync(':memory:');
  sql.exec(fs.readFileSync('cloudflare/schema.sql','utf8'));
  sql.exec(fs.readFileSync('cloudflare/migrations/0003_telemetry_history.sql','utf8'));
  sql.exec(fs.readFileSync('cloudflare/migrations/0004_accounts.sql','utf8'));
  sql.exec(fs.readFileSync('cloudflare/migrations/0005_account_picture.sql','utf8'));
  const DB={prepare(source){const statement=sql.prepare(source);let args=[];
    return {bind(...a){args=a;return this;},async first(){return statement.get(...args) || null;},
      async all(){return {results:statement.all(...args)};},async run(){
        if(statement.columns().length)return {results:statement.all(...args)};
        return {meta:statement.run(...args)};}};},
    async batch(statements){sql.exec('BEGIN');try{const result=[];for(const s of statements)result.push(await s.run());sql.exec('COMMIT');return result;}
      catch(e){sql.exec('ROLLBACK');throw e;}}};
  return {DB,sql};
}
async function setup(){
  const [worker,auth,jose]=await modules, {DB,sql}=database();
  const env={DB,ALLOWED_ORIGIN:frontend,MAYAP_SESSION_PEPPER:'random-session-test-only',DEVICE_KEY_PEPPER:'random-device-test-only',
    GOOGLE_CLIENT_ID:'google-client',MAYAP_MQTT_WSS_URL:'wss://broker.example:8884/mqtt',MAYAP_MQTT_USERNAME:'shared-web',MAYAP_MQTT_PASSWORD:'shared-test-only'};
  async function login(sub){return auth.createSession(env,{sub,email:sub+'@example.test',name:sub});}
  async function device(n){const id='MAP-'+n.toString(16).toUpperCase().padStart(12,'0');
    sql.prepare('INSERT INTO devices(device_id,device_key_hash,created_at,web_pin_hash) VALUES(?,?,?,?)')
      .run(id,'device-key-test',Date.now(),await auth.hash('123456',{MAYAP_SESSION_PEPPER:env.DEVICE_KEY_PEPPER}));return id;}
  async function call(path,sess,data,method=data?'POST':'GET',extra={}){
    return worker.default.fetch(new Request(base+path,{method,headers:{Authorization:sess?'Bearer '+sess.token:'',
      Origin:frontend,'Content-Type':'application/json',...extra},body:data?JSON.stringify(data):undefined}),env,{waitUntil(){}});
  }
  return {env,sql,auth,jose,login,device,call};
}
test('verified account picture persists safely and token expiry stays bounded',async()=>{
  const h=await setup();
  const s=await h.auth.createSession(h.env,{sub:'profile-test',name:'Việt Kiều',email:'user@example.test',picture:'https://lh3.googleusercontent.com/photo'});
  const data=await (await h.call('/api/account/session',s)).json();
  assert.equal(data.user.picture,'https://lh3.googleusercontent.com/photo');
  assert.equal(data.user.name,'Việt Kiều');
  assert.ok(s.expiry-Date.now()<=86400000);
  await h.auth.createSession(h.env,{sub:'profile-test',picture:'https://googleusercontent.com.attacker.test/photo'});
  assert.equal((await (await h.call('/api/account/session',s)).json()).user.picture,'');
  h.sql.prepare('UPDATE user_sessions SET expires_at=? WHERE id=?').run(Date.now()-1,s.id);
  assert.equal((await h.call('/api/account/session',s)).status,401);
});
test('claim checks PIN, refuses takeover, and ownership protects status/history/config/grant',async()=>{
  const h=await setup(), A=await h.login('user-A'), B=await h.login('user-B'), id=await h.device(1);
  assert.equal((await h.call('/api/account/devices/claim',B,{device_id:id,pin:'000000'})).status,403);
  assert.equal((await h.call('/api/account/devices/claim',A,{device_id:id,pin:'123456'})).status,200);
  assert.equal((await h.call('/api/account/devices/claim',B,{device_id:id,pin:'123456'})).status,409);
  for(const route of ['status','history','config'])assert.equal((await h.call(`/api/device/${id}/${route}`,B)).status,403);
  assert.equal((await h.call('/api/device/mqtt-session',B,{device_id:id,control_client_id:'w-browserB123'})).status,403);
  for(const route of ['rename','change-pin'])assert.equal((await h.call('/api/device/'+route,B,{device_id:id,name:'bad',old_pin:'123456',new_pin:'999999'})).status,403);
  assert.equal((await h.call(`/api/device/${id}/status`,A)).status,200);
  assert.equal((await h.call(`/api/device/${id}/status`,null)).status,401);
  assert.equal((await h.call(`/api/device/${id}/status`,null,undefined,'GET',{Cookie:'pairing_token=legacy-known-token'})).status,401);
  assert.equal((await h.call('/api/device/sign-mqtt',A,{device_id:id})).status,410);
  assert.equal((await h.call('/api/device/session-check',A,{device_id:id})).status,410);
});
test('PIN attempts are reserved atomically and bounded across accounts/IPs',async()=>{
  const h=await setup(), A=await h.login('A'), id=await h.device(2);
  const requests=await Promise.all(Array.from({length:6},()=>h.call('/api/account/devices/claim',A,{device_id:id,pin:'000000'})));
  assert.equal(requests.filter(r=>r.status===429).length,1);
  assert.equal(h.sql.prepare('SELECT attempts FROM auth_rate_limits WHERE rate_key=?').get('claim:device:'+id).attempts,6);
});
test('account session uses Google sub and bearer auth, rejects cross origin, logout/expiry/revoke end access',async()=>{
  const h=await setup(), A=await h.login('stable-google-sub'), A2=await h.login('stable-google-sub'), B=await h.login('B');
  const data=await (await h.call('/api/account/session',A)).json();assert.equal(data.user.sub,'stable-google-sub');assert.equal(data.csrf,undefined);
  assert.equal((await h.call('/api/account/logout',null,{},'POST',{Cookie:'mayap_session='+A.token})).status,401);
  assert.equal((await h.call('/api/account/logout',A,{},'POST',{Origin:'https://attacker.example'})).status,403);
  await h.call('/api/account/sessions/revoke',B,{session_id:A2.id});assert.equal((await h.call('/api/account/session',A2)).status,200);
  await h.call('/api/account/sessions/revoke',A,{session_id:A2.id});assert.equal((await h.call('/api/account/session',A2)).status,401);
  const res=await h.call('/api/account/logout',A,{});assert.equal(res.status,200);assert.equal(res.headers.get('Set-Cookie'),null);
  assert.equal((await h.call('/api/account/session',A)).status,401);
  h.sql.prepare('UPDATE user_sessions SET expires_at=? WHERE id=?').run(Date.now()-1,B.id);
  assert.equal((await h.call('/api/account/session',B)).status,401);
});
test('10 customers x 3 devices list only their own machines and receive exact existing HMAC V2 grants',async()=>{
  const h=await setup(), accounts=[],ids=[];
  for(let user=0;user<10;user++){
    const s=await h.login('google-'+user);accounts.push(s);ids[user]=[];
    for(let d=0;d<3;d++){
      const id=await h.device(user*3+d+100);ids[user].push(id);
      assert.equal((await h.call('/api/account/devices/claim',s,{device_id:id,pin:'123456'},'POST',{'CF-Connecting-IP':'192.0.2.'+user})).status,200);
    }
    const rows=await (await h.call('/api/account/session',s)).json();assert.equal(rows.devices.length,3);
    const result=await (await h.call('/api/device/mqtt-session',s,{device_id:ids[user][0],control_client_id:'w-browser'+user+'000'})).json();
    assert.equal(result.success,true);
    assert.equal(result.mqtt.url,h.env.MAYAP_MQTT_WSS_URL);
    assert.equal(result.mqtt.password,h.env.MAYAP_MQTT_PASSWORD);
    const bytes=Buffer.from(await h.auth.hmacHex(h.env.DEVICE_KEY_PEPPER,'mayap-command-key:v1:'+ids[user][0]),'hex');
    assert.equal(result.control.grantSig,await h.auth.hmacHex(bytes,`mayap-control-grant:v2\n${ids[user][0]}\n${result.control.grant}`));
    assert.equal(result.control.sessionKey,await h.auth.hmacHex(bytes,`mayap-control-session:v2\n${ids[user][0]}\n${result.control.grant}`));
    assert.match(result.control.grant,new RegExp('^w-browser'+user+'000\\|'));

  }
  for(let i=0;i<10;i++)for(let j=0;j<10;j++)if(i!==j){
    assert.equal((await h.call(`/api/device/${ids[j][0]}/status`,accounts[i])).status,403);
    assert.equal((await h.call('/api/device/mqtt-session',accounts[i],{device_id:ids[j][1],control_client_id:'w-cross12345'})).status,403);
  }
  assert.equal(h.sql.prepare('SELECT COUNT(*) AS n FROM user_devices').get().n,30);
});
test('direct HiveMQ credentials require account membership and configured existing broker password',async()=>{
  const h=await setup(),A=await h.login('A'),id=await h.device(8);
  await h.call('/api/account/devices/claim',A,{device_id:id,pin:'123456'});
  h.env.MAYAP_MQTT_PASSWORD='';
  assert.equal((await h.call('/api/device/mqtt-session',A,{device_id:id,control_client_id:'w-client000'})).status,503);
});
test('Google RS256 identity validates signature, audience, issuer, expiry, nonce, azp and sub',async()=>{
  const [,auth,jose]=await modules, pair=await jose.generateKeyPair('RS256');
  const mint=(claims={},key=pair.privateKey)=>new jose.SignJWT({nonce:'expected',...claims}).setProtectedHeader({alg:'RS256'})
    .setIssuer(claims.iss || 'https://accounts.google.com').setAudience(claims.aud || 'google-client')
    .setSubject(claims.sub || 'google-sub').setIssuedAt().setExpirationTime(claims.exp || '5m').sign(key);
  const good=await mint({email:'can-change@example.test'});assert.equal((await auth.verifyGoogle(good,'google-client','expected',pair.publicKey)).sub,'google-sub');
  for(const claims of [{aud:'wrong'},{iss:'https://attacker.example'},{exp:Math.floor(Date.now()/1000)-60},{nonce:'wrong'},{azp:'wrong'}])
    await assert.rejects(auth.verifyGoogle(await mint(claims),'google-client','expected',pair.publicKey));
  const other=await jose.generateKeyPair('RS256');await assert.rejects(auth.verifyGoogle(await mint({},other.privateKey),'google-client','expected',pair.publicKey));
  await assert.rejects(auth.verifyGoogle('invalid','google-client','expected',pair.publicKey));
});
test('Google nonce challenge is one-use, expired/forged challenges cannot create sessions',async()=>{
  const h=await setup();
  const challenge=await (await h.call('/api/account/google/challenge',null,{})).json();
  assert.match(challenge.nonce,/^[a-f0-9]{64}$/);assert.equal(challenge.clientId,h.env.GOOGLE_CLIENT_ID);
  assert.equal((await h.call('/api/account/google/login',null,{challenge:challenge.challenge,credential:'invalid'})).status,401);
  assert.equal(h.sql.prepare('SELECT COUNT(*) AS n FROM google_login_challenges').get().n,0);
  assert.equal((await h.call('/api/account/google/login',null,{challenge:challenge.challenge,credential:'invalid'})).status,401);
  const next=await (await h.call('/api/account/google/challenge',null,{})).json();
  h.sql.prepare('UPDATE google_login_challenges SET expires_at=?').run(Date.now()-1);
  assert.equal((await h.call('/api/account/google/login',null,{challenge:next.challenge,credential:'invalid'})).status,401);
});
test('GIS login verifies remote Google JWKS and issues a hash-only revocable MAYAP bearer session',async()=>{
  const h=await setup(),keys=await h.jose.generateKeyPair('RS256',{extractable:true});
  const challenge=await (await h.call('/api/account/google/challenge',null,{})).json();
  const jwk=await h.jose.exportJWK(keys.publicKey);jwk.kid='test-google-key';jwk.alg='RS256';jwk.use='sig';
  const token=await new h.jose.SignJWT({nonce:challenge.nonce,email:'display@example.test',name:'Google user'})
    .setProtectedHeader({alg:'RS256',kid:jwk.kid}).setIssuer('https://accounts.google.com')
    .setAudience(h.env.GOOGLE_CLIENT_ID).setSubject('actual-google-sub').setIssuedAt().setExpirationTime('5m').sign(keys.privateKey);
  const original=global.fetch;global.fetch=async url=>{
    assert.equal(String(url),'https://www.googleapis.com/oauth2/v3/certs');return new Response(JSON.stringify({keys:[jwk]}));
  };
  try {
    const res=await h.call('/api/account/google/login',null,{challenge:challenge.challenge,credential:token});
    assert.equal(res.status,200);const data=await res.json();assert.match(data.token,/^[a-f0-9]{64}$/);
    const saved=h.sql.prepare('SELECT * FROM user_sessions').get();assert.equal(saved.user_sub,'actual-google-sub');
    assert.notEqual(saved.token_hash,data.token);assert.equal(saved.token_hash,await h.auth.hash(data.token,h.env));
    assert.ok(data.expiresAt>Date.now()+86300000);assert.ok(data.expiresAt<=Date.now()+86400000);
    assert.equal((await h.call('/api/account/session',{token:data.token})).status,200);
    assert.equal((await h.call('/api/account/google/login',null,{challenge:challenge.challenge,credential:token})).status,401);
  }finally{global.fetch=original;}
});
test('exact-origin CORS preflight allows bearer JSON API calls, denies other origins/headers',async()=>{
  const h=await setup();
  const good=await h.call('/api/account/session',null,undefined,'OPTIONS',{'Access-Control-Request-Method':'GET','Access-Control-Request-Headers':'authorization,content-type'});
  assert.equal(good.status,204);assert.equal(good.headers.get('Access-Control-Allow-Origin'),frontend);
  assert.equal((await h.call('/api/account/session',null,undefined,'OPTIONS',{Origin:'https://attacker.example','Access-Control-Request-Method':'GET'})).status,403);
  assert.equal((await h.call('/api/account/session',null,undefined,'OPTIONS',{'Access-Control-Request-Method':'POST','Access-Control-Request-Headers':'x-unexpected'})).status,403);
});
test('viewer receives existing shared broker credentials but cannot obtain control grant or change devices via API',async()=>{
  const h=await setup(),owner=await h.login('owner'),viewer=await h.login('viewer'),id=await h.device(50);
  await h.call('/api/account/devices/claim',owner,{device_id:id,pin:'123456'});
  h.sql.prepare("INSERT INTO user_devices(user_sub,device_id,role,created_at) VALUES(?,?,'viewer',?)").run('viewer',id,Date.now());
  const data=await (await h.call('/api/device/mqtt-session',viewer,{device_id:id,control_client_id:'w-viewer000'})).json();
  assert.equal(data.control,null);
  assert.equal(data.mqtt.password,h.env.MAYAP_MQTT_PASSWORD); // No claim of broker read isolation.
  assert.equal((await h.call('/api/device/rename',viewer,{device_id:id,name:'bad'})).status,403);
  assert.equal((await h.call('/api/device/change-pin',viewer,{device_id:id,old_pin:'123456',new_pin:'999999'})).status,403);
});
test('reviewed single-C512 firmware bridge and 300-second WARM stay protected',()=>{
  const crypto=require('node:crypto');
  // Reviewed storage geometry/history and final EEPROM-backed reminder report only.
  // Account auth/grants and signed transaction bodies retain their other guards.
  const baseline={"config.h": "b62cb070e0ee1f6c26ebdd736854e673d82360bbc554808416d974457f2640bd", "realtime_link.h": "516483669ee72ffa1d4e4d4ee269813975cd16c149079399823d2280a6ab182f"};
  for(const file of ['config.h','realtime_link.h']) {
    const filename='MAYAP_INDUSTRIAL_v4_0_0/'+file;
    assert.equal(crypto.createHash('sha256').update(fs.readFileSync(filename,'utf8').replace(/\r\n/g,'\n')).digest('hex'),baseline[file]);
  }
  assert.match(fs.readFileSync('app.js','utf8'),/WARM_BACKGROUND_MS = 300000/);
  assert.ok(!fs.readFileSync('config.js','utf8').includes('session-check'));
  assert.ok(!fs.existsSync('mqtt-gateway/server.js'));
});
test('push ownership is account-scoped and a revoked session cannot receive device alerts',async()=>{
  const h=await setup(),A=await h.login('push-A'),B=await h.login('push-B'),idA=await h.device(70),idB=await h.device(71);
  await h.call('/api/account/devices/claim',A,{device_id:idA,pin:'123456'});
  await h.call('/api/account/devices/claim',B,{device_id:idB,pin:'123456'});
  const subscription={endpoint:'https://push.example/account-A',keys:{p256dh:'test',auth:'test'}};
  assert.equal((await h.call('/api/push/subscribe',A,{device_id:idA,subscription})).status,200);
  assert.equal((await h.call('/api/push/subscribe',B,{device_id:idA,subscription})).status,403);
  assert.equal((await h.call('/api/push/subscribe',B,{device_id:idB,subscription})).status,403);
  const account=await (await h.call('/api/account/session',A)).json();assert.equal(account.devices[0].linked_browsers,1);
  await h.call('/api/account/logout',A,{});assert.equal(h.sql.prepare('SELECT COUNT(*) AS n FROM push_subscriptions').get().n,0);
  assert.equal((await h.call('/api/push/subscribe',A,{device_id:idA,subscription})).status,401);
});
