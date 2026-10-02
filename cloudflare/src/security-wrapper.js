// Physical-device admission only. Account authorization lives in account-worker.js.
import worker from './index.js';
const DEVICE_ID_RE = /^MAP-[A-F0-9]{12}$/;
const DEVICE_KEY_RE = /^[A-Fa-f0-9]{64}$/;
const json = (env, data, status=200) => new Response(JSON.stringify(data), {
  status, headers:{'Content-Type':'application/json; charset=utf-8','Cache-Control':'no-store'}
});
async function readJson(request) { try {return await request.clone().json();} catch {return null;} }
async function delegate(request,env,ctx) {return worker.fetch(request,env,ctx);}
async function handleSecureRegister(request, env, ctx) {
  const body = await readJson(request);
  const deviceId = String(body?.device_id || '').trim();
  const deviceKey = String(body?.device_key || '');
  if (!DEVICE_ID_RE.test(deviceId) || !DEVICE_KEY_RE.test(deviceKey)) {
    return json(env, { success: false, error: 'device_id/device_key khong hop le' }, 400);
  }
  const existing = await env.DB.prepare('SELECT 1 AS ok FROM devices WHERE device_id = ?1').bind(deviceId).first();
  if (!existing) {
    const inventory = await env.DB.prepare(
      'SELECT enabled FROM device_inventory WHERE device_id = ?1'
    ).bind(deviceId).first();
    if (Number(inventory?.enabled || 0) !== 1) {
      return json(env, { success: false, error: 'Thiết bị chưa được cấp phép xuất xưởng' }, 403);
    }
  }
  return delegate(request, env, ctx);
}


export default {
  async scheduled(event,env,ctx) {return worker.scheduled(event,env,ctx);},
  async fetch(request,env,ctx) {
    if(new URL(request.url).pathname==='/api/device/register' && request.method==='POST')
      return handleSecureRegister(request,env,ctx);
    return delegate(request,env,ctx);
  }
};
