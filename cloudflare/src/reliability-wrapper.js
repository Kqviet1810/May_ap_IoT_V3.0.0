import worker from './security-wrapper.js';

// MAYAP v3.8.1 reliability wrapper
//
// Muc tieu:
// 1) May moi co the tu provisioning o quy mo nho/vua ma khong can nguoi lap
//    dat INSERT device_id vao D1 bang tay.
// 2) Van giu device_inventory de co the khoa mot may cu the hoac bat lai che
//    do factory allowlist sau nay chi bang bien REQUIRE_DEVICE_INVENTORY=1.
// 3) Gioi han toc do tao may moi theo IP de tranh spam D1.
// 4) Reset PIN tu HMI co the tu phuc hoi truong hop may chua tung register
//    thanh cong (loi deployment/allowlist cu), thay vi mac ket 404 mai mai.
//
// Wrapper nay KHONG thay doi cac kiem tra device_key, PIN, browser session,
// MQTT signing hay push cua security-wrapper.js. Sau buoc admission, request
// van phai di qua toan bo lop bao ve hien co.

const DEVICE_ID_RE = /^MAP-[A-F0-9]{12}$/;
const DEVICE_KEY_RE = /^[A-Fa-f0-9]{64}$/;

function strictInventoryRequired(env) {
  return String(env.REQUIRE_DEVICE_INVENTORY || '0') === '1';
}

function registrationLimit(env) {
  const parsed = Number.parseInt(String(env.MAX_NEW_DEVICE_REGISTRATIONS_PER_HOUR || '20'), 10);
  return Number.isFinite(parsed) ? Math.min(200, Math.max(1, parsed)) : 20;
}

function registrationWindowMs(env) {
  const parsed = Number.parseInt(String(env.NEW_DEVICE_REGISTRATION_WINDOW_MINUTES || '60'), 10);
  const minutes = Number.isFinite(parsed) ? Math.min(1440, Math.max(1, parsed)) : 60;
  return minutes * 60 * 1000;
}

function corsHeaders(env) {
  return {
    'Access-Control-Allow-Origin': env.ALLOWED_ORIGIN || '*',
    'Access-Control-Allow-Methods': 'GET, POST, DELETE, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type',
    'Access-Control-Max-Age': '86400',
  };
}

function json(env, data, status = 200, extraHeaders = {}) {
  return new Response(JSON.stringify(data), {
    status,
    headers: {
      'Content-Type': 'application/json; charset=utf-8',
      ...corsHeaders(env),
      ...extraHeaders,
    },
  });
}

async function readJson(request) {
  try {
    return await request.clone().json();
  } catch (_) {
    return null;
  }
}

function validDeviceCredentials(body) {
  return DEVICE_ID_RE.test(String(body?.device_id || '').trim()) &&
    DEVICE_KEY_RE.test(String(body?.device_key || ''));
}

function registerRateKey(request) {
  const ip = request.headers.get('CF-Connecting-IP') || 'unknown';
  return `register:${ip}`;
}

async function registrationAllowed(request, env, now) {
  const key = registerRateKey(request);
  const windowMs = registrationWindowMs(env);
  const row = await env.DB.prepare(
    'SELECT attempts, window_started_at FROM auth_rate_limits WHERE rate_key = ?1'
  ).bind(key).first();

  if (!row) return { allowed: true, key, attempts: 0, startedAt: now, windowMs };

  const startedAt = Number(row.window_started_at || 0);
  if (!startedAt || now - startedAt >= windowMs) {
    await env.DB.prepare('DELETE FROM auth_rate_limits WHERE rate_key = ?1').bind(key).run();
    return { allowed: true, key, attempts: 0, startedAt: now, windowMs };
  }

  const attempts = Number(row.attempts || 0);
  const limit = registrationLimit(env);
  if (attempts >= limit) {
    return {
      allowed: false,
      key,
      attempts,
      startedAt,
      windowMs,
      retryAfterMs: Math.max(1000, windowMs - (now - startedAt)),
    };
  }
  return { allowed: true, key, attempts, startedAt, windowMs };
}

async function recordNewDeviceAdmission(env, rate, now) {
  const attempts = Number(rate.attempts || 0) + 1;
  const startedAt = Number(rate.attempts || 0) === 0 ? now : Number(rate.startedAt || now);
  await env.DB.prepare(
    `INSERT INTO auth_rate_limits
      (rate_key, attempts, window_started_at, blocked_until, updated_at)
     VALUES (?1, ?2, ?3, 0, ?4)
     ON CONFLICT(rate_key) DO UPDATE SET
       attempts = excluded.attempts,
       window_started_at = excluded.window_started_at,
       blocked_until = 0,
       updated_at = excluded.updated_at`
  ).bind(rate.key, attempts, startedAt, now).run();
}

async function ensureNewDeviceAdmitted(request, env, body) {
  const deviceId = String(body.device_id).trim();

  // Da la may da dang ky: khong can admission nua. security-wrapper/index.js
  // se tiep tuc kiem device_key cu nhu truoc, nen khong co duong chiem lai ID.
  const existing = await env.DB.prepare(
    'SELECT 1 AS ok FROM devices WHERE device_id = ?1'
  ).bind(deviceId).first();
  if (existing) return { ok: true, existing: true };

  // Neu inventory co ban ghi explicit thi ton trong no. enabled=0 la cach
  // khoa xuat xuong/blacklist mot ID ngay ca khi auto provisioning dang bat.
  const inventory = await env.DB.prepare(
    'SELECT enabled FROM device_inventory WHERE device_id = ?1'
  ).bind(deviceId).first();
  if (inventory) {
    if (Number(inventory.enabled || 0) !== 1) {
      return { ok: false, response: json(env, {
        success: false,
        error: 'Thiết bị đã bị khóa trong danh sách xuất xưởng',
      }, 403) };
    }
    return { ok: true, existing: false, inventory: true };
  }

  // Che do nghiem ngat cho giai doan san xuat lon: hanh vi cu duoc giu nguyen.
  if (strictInventoryRequired(env)) {
    return { ok: false, response: json(env, {
      success: false,
      error: 'Thiết bị chưa được cấp phép xuất xưởng',
    }, 403) };
  }

  const now = Date.now();
  const rate = await registrationAllowed(request, env, now);
  if (!rate.allowed) {
    return { ok: false, response: json(env, {
      success: false,
      error: 'Tạm giới hạn tạo thiết bị mới. Hãy thử lại sau.',
    }, 429, { 'Retry-After': String(Math.ceil(rate.retryAfterMs / 1000)) }) };
  }

  // Tu admission vao inventory de security-wrapper ben trong van la nguon
  // kiem tra duy nhat. INSERT OR IGNORE xu ly an toan neu 2 request den gan nhau.
  await env.DB.prepare(
    `INSERT OR IGNORE INTO device_inventory
      (device_id, enabled, browser_limit, created_at)
     VALUES (?1, 1, NULL, ?2)`
  ).bind(deviceId, now).run();
  await recordNewDeviceAdmission(env, rate, now);

  return { ok: true, existing: false, autoAdmitted: true };
}

async function handleRegister(request, env, ctx) {
  const body = await readJson(request);
  if (!validDeviceCredentials(body)) {
    // De security-wrapper tra loi validation chuan cua he thong.
    return worker.fetch(request, env, ctx);
  }

  const admission = await ensureNewDeviceAdmitted(request, env, body);
  if (!admission.ok) return admission.response;
  return worker.fetch(request, env, ctx);
}

async function handleResetPin(request, env, ctx) {
  const body = await readJson(request);
  if (!validDeviceCredentials(body)) return worker.fetch(request, env, ctx);

  const deviceId = String(body.device_id).trim();
  const existing = await env.DB.prepare(
    'SELECT 1 AS ok FROM devices WHERE device_id = ?1'
  ).bind(deviceId).first();
  if (existing) return worker.fetch(request, env, ctx);

  // Self-heal cho may moi bi mac o "DANG DONG BO": neu chua co ban ghi devices,
  // Reset PIN tai HMI se provisioning may truoc, va tra PIN moi ve ngay trong
  // chinh response reset-pin. Device key van phai dung 64-hex va register van
  // qua security-wrapper, nen khong ha cap xac thuc.
  const admission = await ensureNewDeviceAdmitted(request, env, body);
  if (!admission.ok) return admission.response;

  const registerUrl = new URL(request.url);
  registerUrl.pathname = '/api/device/register';
  const registerRequest = new Request(registerUrl.toString(), {
    method: 'POST',
    headers: request.headers,
    body: JSON.stringify({
      device_id: deviceId,
      device_key: String(body.device_key),
      device_name: deviceId,
    }),
  });
  const response = await worker.fetch(registerRequest, env, ctx);
  if (!response.ok) return response;

  const data = await response.json().catch(() => null);
  if (!data?.success || !data?.web_pin) {
    return json(env, {
      success: false,
      error: 'Đã đăng ký máy nhưng chưa nhận được PIN mới',
    }, 503);
  }

  return json(env, {
    success: true,
    web_pin: data.web_pin,
    provisioned: true,
  });
}

export default {
  async scheduled(event, env, ctx) {
    if (worker.scheduled) return worker.scheduled(event, env, ctx);
  },

  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const path = url.pathname;

    if (path === '/api/device/register' && request.method === 'POST') {
      return handleRegister(request, env, ctx);
    }
    if (path === '/api/device/reset-pin' && request.method === 'POST') {
      return handleResetPin(request, env, ctx);
    }
    return worker.fetch(request, env, ctx);
  },
};
