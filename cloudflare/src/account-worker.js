import physicalWorker from './reliability-wrapper.js';
import legacy from './index.js';
import { randomToken, verifyDeviceKey, timingSafeEqual } from './auth.js';
import { hash, json, session,
  deviceList, permission, createSession, verifyGoogle, mqttCredentials, controlGrant } from './account-auth.js';

const idRe = /^MAP-[A-F0-9]{12}$/;
const physical = new Set(['/api/device/register','/api/device/heartbeat','/api/device/reset-pin',
  '/api/device/rotate-key','/api/device/alarm','/api/firmware/check']);
const deny = (status=403) => json({ success:false, error:status===401?'ACCOUNT_LOGIN_REQUIRED':'ACCESS_DENIED' },status);
async function body(request) {
  if (Number(request.headers.get('Content-Length') || 0)>8192) throw new Error('BODY_TOO_LARGE');
  const text=await boundedText(request,8192);
  const value=JSON.parse(text);
  if (!value || typeof value!=='object' || Array.isArray(value)) throw new Error('INVALID_BODY');
  return value;
}
async function boundedText(request,limit) {
  if(!request.body)return '';
  const reader=request.body.getReader(), chunks=[];let size=0;
  try {
    for(;;){const {value,done}=await reader.read();if(done)break;size+=value.byteLength;
      if(size>limit){await reader.cancel();throw new Error('BODY_TOO_LARGE');}chunks.push(value);}
  }finally{reader.releaseLock();}
  const bytes=new Uint8Array(size);let offset=0;for(const chunk of chunks){bytes.set(chunk,offset);offset+=chunk.byteLength;}
  return new TextDecoder().decode(bytes);
}
function allowedOrigin(env) {
  const value=env.ALLOWED_ORIGIN;
  const url=new URL(value);
  if(url.protocol!=='https:' || url.origin!==value) throw new Error('ALLOWED_ORIGIN_INVALID');
  return value;
}
async function loginChallenge(request,env) {
  if(!env.GOOGLE_CLIENT_ID || !env.MAYAP_SESSION_PEPPER)
    return json({success:false,error:'GOOGLE_LOGIN_NOT_CONFIGURED'},503);
  const now=Date.now(), key='google:ip:'+(request.headers.get('CF-Connecting-IP') || 'unknown');
  const row=await env.DB.prepare(`INSERT INTO auth_rate_limits
    (rate_key,attempts,window_started_at,blocked_until,updated_at) VALUES(?,1,?,0,?)
    ON CONFLICT(rate_key) DO UPDATE SET
    attempts=CASE WHEN window_started_at<? THEN 1 ELSE attempts+1 END,
    window_started_at=CASE WHEN window_started_at<? THEN excluded.window_started_at ELSE window_started_at END,
    updated_at=excluded.updated_at RETURNING attempts`).bind(key,now,now,now-900000,now-900000).first();
  if(row.attempts>60)return json({success:false,error:'LOGIN_RATE_LIMIT'},429);
  const challenge=randomToken(32), nonce=randomToken(32);
  await env.DB.prepare('INSERT INTO google_login_challenges(challenge_hash,nonce,expires_at) VALUES(?,?,?)')
    .bind(await hash(challenge,env),nonce,now+300000).run();
  return json({success:true,challenge,nonce,clientId:env.GOOGLE_CLIENT_ID});
}
async function loginGoogle(request,env) {
  const data=await body(request);
  if(!/^[a-f0-9]{64}$/.test(data.challenge || '') || typeof data.credential!=='string')return deny(401);
  const tx=await env.DB.prepare('DELETE FROM google_login_challenges WHERE challenge_hash=? AND expires_at>? RETURNING *')
    .bind(await hash(data.challenge,env),Date.now()).first();
  if(!tx)return deny(401);
  let identity;
  try {identity=await verifyGoogle(data.credential,env.GOOGLE_CLIENT_ID,tx.nonce);} catch {return deny(401);}
  const old=await session(request,env);
  if(old)await revoke(env,old.id,old.user_sub);
  const own=await createSession(env,identity,request.headers.get('User-Agent') || '');
  return json({success:true,token:own.token,expiresAt:own.expiry});
}
async function revoke(env,id,sub) {
  await env.DB.batch([
    env.DB.prepare('UPDATE user_sessions SET revoked_at=? WHERE id=? AND user_sub=?').bind(Date.now(),id,sub),
    env.DB.prepare('DELETE FROM push_subscriptions WHERE user_session_id=? AND user_sub=?').bind(id,sub),
  ]);
}
async function claim(request, env, auth, data) {
  const id=String(data.device_id || '').toUpperCase(), pin=String(data.pin || '');
  if (!idRe.test(id) || !/^[0-9]{4,8}$/.test(pin)) return json({success:false,error:'INVALID_CLAIM'},400);
  const now=Date.now(), ip=request.headers.get('CF-Connecting-IP') || 'unknown';
  // Reserve attempts atomically BEFORE verifying. Parallel requests cannot skip the limit.
  const keys=[`claim:device:${id}`,`claim:ip:${ip}`];
  const rows=await env.DB.batch(keys.map(key=>env.DB.prepare(`INSERT INTO auth_rate_limits
    (rate_key,attempts,window_started_at,blocked_until,updated_at) VALUES(?,1,?,0,?)
    ON CONFLICT(rate_key) DO UPDATE SET
    attempts=CASE WHEN window_started_at<? THEN 1 ELSE attempts+1 END,
    window_started_at=CASE WHEN window_started_at<? THEN excluded.window_started_at ELSE window_started_at END,
    updated_at=excluded.updated_at RETURNING attempts`).bind(key,now,now,now-900000,now-900000)));
  if (rows[0].results[0].attempts>5 || rows[1].results[0].attempts>30)
    return json({success:false,error:'Thử quá nhiều lần. Vui lòng chờ 15 phút.'},429);
  const device=await env.DB.prepare(`SELECT d.* FROM devices d LEFT JOIN device_inventory i ON i.device_id=d.device_id
    WHERE d.device_id=? AND COALESCE(i.enabled,1)=1`).bind(id).first();
  if (!device || !env.DEVICE_KEY_PEPPER || !await verifyDeviceKey(pin,env.DEVICE_KEY_PEPPER,device.web_pin_hash))
    return json({success:false,error:'Device ID hoặc PIN không đúng.'},403);
  await env.DB.prepare(`INSERT OR IGNORE INTO user_devices(user_sub,device_id,role,created_at)
    SELECT ?,?,'owner',? WHERE NOT EXISTS(SELECT 1 FROM user_devices WHERE device_id=? AND role='owner')`)
    .bind(auth.user_sub,id,now,id).run();
  const owner=await env.DB.prepare("SELECT user_sub FROM user_devices WHERE device_id=? AND role='owner'").bind(id).first();
  if (owner?.user_sub!==auth.user_sub) return json({success:false,error:'Thiết bị đã thuộc một tài khoản khác.'},409);
  return json({success:true,device_id:id,device_name:device.device_name || id});
}
async function fetchAccount(request, env, ctx) {
  const url=new URL(request.url), path=url.pathname, method=request.method;
  // Existing physical-device authentication and task architecture stay unchanged.
  if ((method==='POST' && physical.has(path)) || (method==='GET' && /^\/api\/firmware\/download\//.test(path)))
    return physicalWorker.fetch(request,env,ctx);
  if(request.headers.get('Origin')!==allowedOrigin(env))return deny();
  if(method==='OPTIONS') {
    if(!['GET','POST','DELETE'].includes(request.headers.get('Access-Control-Request-Method')))return deny();
    const requested=(request.headers.get('Access-Control-Request-Headers') || '').toLowerCase().split(',').map(x=>x.trim()).filter(Boolean);
    if(requested.some(x=>!['authorization','content-type'].includes(x)))return deny();
    return new Response(null,{status:204,headers:{'Access-Control-Allow-Methods':'GET, POST, DELETE',
      'Access-Control-Allow-Headers':'Authorization, Content-Type','Access-Control-Max-Age':'600'}});
  }
  if(path==='/api/account/google/challenge' && method==='POST')return loginChallenge(request,env);
  if(path==='/api/account/google/login' && method==='POST')return loginGoogle(request,env);
  if(!path.startsWith('/api/'))return json({success:false,error:'NOT_FOUND'},404);
  // No permissive CORS or legacy browser credential fallback.
  if (path==='/api/device/sign-mqtt' || path==='/api/device/verify-pin' || path==='/api/device/session-check')
    return json({success:false,error:'ACCOUNT_UPGRADE_REQUIRED'},410);
  const auth=await session(request,env);
  if (!auth) return deny(401);
  if (path==='/api/account/session' && method==='GET') return json({success:true,
    user:{sub:auth.user_sub,name:auth.name,email:auth.email,picture:auth.picture},expiresAt:auth.expires_at,
    devices:await deviceList(env,auth.user_sub)});
  const data=method==='GET' ? {} : await body(request);
  if (path==='/api/account/logout' && method==='POST') {
    await revoke(env,auth.id,auth.user_sub);
    return json({success:true});
  }
  if (path==='/api/account/sessions' && method==='GET') {
    const {results}=await env.DB.prepare('SELECT id,created_at,expires_at,user_agent FROM user_sessions WHERE user_sub=? AND revoked_at IS NULL AND expires_at>?')
      .bind(auth.user_sub,Date.now()).all(); return json({success:true,sessions:results});
  }
  if (path==='/api/account/sessions/revoke' && method==='POST') {
    await revoke(env,String(data.session_id || ''),auth.user_sub); return json({success:true});
  }
  if (path==='/api/account/devices/claim' && method==='POST') return claim(request,env,auth,data);
  if (path==='/api/push/vapid-public-key' && method==='GET') return legacy.fetch(request,env,ctx);
  if (path==='/api/firmware/latest' && method==='GET') return legacy.fetch(request,env,ctx);
  const match=path.match(/^\/api\/device\/(MAP-[A-F0-9]{12})\/(status|history|config)$/);
  const id=match?.[1] || String(data.device_id || '');
  if (match && method==='GET') {
    const device=await permission(env,auth.user_sub,id);
    if (!device) return deny();
    if (match[2]==='config') return json({success:false,error:'LOAD_CONFIG_OVER_AUTHORIZED_MQTT'},409);
    if (match[2]==='history') {
      const {results}=await env.DB.prepare('SELECT recorded_at,temperature,humidity,batch_running FROM telemetry_history WHERE device_id=? ORDER BY recorded_at DESC LIMIT 500').bind(id).all();
      return json({success:true,points:results});
    }
    const subs=await env.DB.prepare('SELECT COUNT(*) AS n FROM push_subscriptions WHERE device_id=? AND user_sub=?').bind(id,auth.user_sub).first();
    return json({success:true,exists:true,device_id:id,device_name:device.device_name,status:device.status,last_seen:device.last_seen,
      batch_running:!!device.batch_running,subscription_count:subs.n});
  }
  if (path==='/api/device/mqtt-session' && method==='POST') {
    const device=await permission(env,auth.user_sub,id), cid=String(data.control_client_id || '');
    if (!device) return deny();
    if (!/^[A-Za-z0-9_-]{8,40}$/.test(cid)) return json({success:false,error:'INVALID_CLIENT'},400);
    try { return json({success:true,mqtt:mqttCredentials(env),
      control:device.role==='viewer' ? null : await controlGrant(env,id,cid)}); }
    catch { return json({success:false,error:'MQTT_NOT_CONFIGURED'},503); }
  }
  if (path==='/api/device/rename' && method==='POST') {
    const device=await permission(env,auth.user_sub,id,true);
    if (!device || device.role!=='owner') return deny();
    const name=String(data.device_name || data.name || '').trim().slice(0,64);
    if (!name) return json({success:false},400);
    await env.DB.prepare('UPDATE devices SET device_name=? WHERE device_id=?').bind(name,id).run();
    return json({success:true,device_name:name});
  }
  if (path==='/api/device/change-pin' && method==='POST') {
    const device=await permission(env,auth.user_sub,id,true);
    if (!device || device.role!=='owner') return deny();
    // Legacy handler still verifies old PIN; never returns the legacy pairing secret to Web.
    const response=await legacy.fetch(new Request(request.url,{method:'POST',headers:request.headers,
      body:JSON.stringify(data)}),env,ctx), value=await response.json(); delete value.pairing_token;
    return json(value,response.status);
  }
  if (path==='/api/push/subscribe' && method==='POST') {
    const device=await permission(env,auth.user_sub,id), sub=data.subscription;
    if (!device) return deny();
    if (!sub?.endpoint?.startsWith('https://') || !sub.keys?.auth || !sub.keys?.p256dh) return json({success:false},400);
    const existing=await env.DB.prepare('SELECT user_sub FROM push_subscriptions WHERE endpoint=? LIMIT 1').bind(sub.endpoint).first();
    if (existing && existing.user_sub!==auth.user_sub) return deny();
    await env.DB.prepare(`INSERT INTO push_subscriptions(device_id,endpoint,p256dh,auth,user_agent,created_at,updated_at,user_sub,user_session_id)
      VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(device_id,endpoint) DO UPDATE SET p256dh=excluded.p256dh,
      auth=excluded.auth,updated_at=excluded.updated_at,user_session_id=excluded.user_session_id`)
      .bind(id,sub.endpoint,sub.keys.p256dh,sub.keys.auth,request.headers.get('User-Agent') || '',Date.now(),Date.now(),auth.user_sub,auth.id).run();
    return json({success:true});
  }
  if ((path==='/api/push/subscribe' && method==='DELETE') || (path==='/api/push/test' && method==='POST')) {
    const sub=await env.DB.prepare(`SELECT ps.* FROM push_subscriptions ps JOIN user_devices ud
      ON ud.device_id=ps.device_id AND ud.user_sub=ps.user_sub WHERE ps.endpoint=? AND ps.user_sub=? LIMIT 1`)
      .bind(String(data.endpoint || ''),auth.user_sub).first();
    if(!sub) return deny();
    if(method==='DELETE') {
      await env.DB.prepare('DELETE FROM push_subscriptions WHERE endpoint=? AND user_sub=?').bind(data.endpoint,auth.user_sub).run();
      return json({success:true});
    }
    return legacy.fetch(new Request(request.url,{method,headers:request.headers,body:JSON.stringify(data)}),env,ctx);
  }
  return json({success:false,error:'NOT_FOUND'},404);
}
export default {
  async fetch(request,env,ctx) {
    let response;
    try {response=await fetchAccount(request,env,ctx);} catch(error) {
      const bad=['INVALID_BODY','BODY_TOO_LARGE'].includes(error.message) || error instanceof SyntaxError;
      response=json({success:false,error:bad?'INVALID_REQUEST':'SERVICE_UNAVAILABLE'},bad?400:503);
    }
    // Account APIs use exact-origin CORS + explicit bearer authorization; no third-party cookies.
    if(request.headers.get('Origin')===env.ALLOWED_ORIGIN) {
      const headers=new Headers(response.headers);headers.set('Access-Control-Allow-Origin',env.ALLOWED_ORIGIN);
      headers.set('Vary','Origin');return new Response(response.body,{status:response.status,headers});
    }
    return response;
  },
  async scheduled(event,env,ctx) {
    await env.DB.batch([
      env.DB.prepare('DELETE FROM google_login_challenges WHERE expires_at<?').bind(Date.now()),
      env.DB.prepare('DELETE FROM push_subscriptions WHERE user_session_id IN (SELECT id FROM user_sessions WHERE revoked_at IS NOT NULL OR expires_at<?)').bind(Date.now()),
    ]);
    return physicalWorker.scheduled(event,env,ctx);
  },
};
