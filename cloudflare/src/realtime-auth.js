import { hmacHex } from './account-auth.js';
import { randomToken, timingSafeEqual } from './auth.js';

export const DEVICE_ID = /^MAP-[A-F0-9]{12}$/;
export const CLIENT_ID = /^[A-Za-z0-9_-]{8,39}$/;
export const TICKET_TTL_SEC = 60;
export const CONNECTION_LEASE_MS = 300000;
export const FRAME_CAP = 2048;
export const WRITE_CHANNELS = new Set(['command', 'config/set', 'history/request', 'notes/request']);
export const DEVICE_CHANNELS = new Set(['presence', 'bootstrap', 'snapshot', 'config/reported',
  'reminders/reported', 'ack', 'log', 'history/reported', 'notes/reported']);
const encoder = new TextEncoder();

export async function issueTicket(env, claims, now = Date.now()) {
  if (!env.MAYAP_SESSION_PEPPER) throw new Error('REALTIME_NOT_CONFIGURED');
  const bytes = encoder.encode(JSON.stringify({ ...claims, exp: Math.floor(now / 1000) + TICKET_TTL_SEC,
    nonce: randomToken(16) }));
  const body = btoa(String.fromCharCode(...bytes)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  return `${body}.${await hmacHex(env.MAYAP_SESSION_PEPPER, 'mayap-realtime-ticket:v1\n' + body)}`;
}
export async function verifyTicket(env, ticket, now = Date.now()) {
  if (typeof ticket !== 'string' || ticket.length > 1400 || !env.MAYAP_SESSION_PEPPER) return null;
  const parts = ticket.split('.');
  if (parts.length !== 2 || !/^[A-Za-z0-9_-]+$/.test(parts[0]) || !/^[a-f0-9]{64}$/.test(parts[1])) return null;
  if (!timingSafeEqual(parts[1], await hmacHex(env.MAYAP_SESSION_PEPPER, 'mayap-realtime-ticket:v1\n' + parts[0]))) return null;
  try {
    const raw = atob(parts[0].replace(/-/g, '+').replace(/_/g, '/'));
    const value = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Uint8Array.from(raw, c => c.charCodeAt(0))));
    const sec = Math.floor(now / 1000);
    if (value.aud !== 'browser' || !DEVICE_ID.test(value.deviceId) || !CLIENT_ID.test(value.clientId) ||
        !/^[a-f0-9]{48}$/.test(value.sessionId) || typeof value.userSub !== 'string' ||
        !['owner', 'operator', 'viewer'].includes(value.role) || !/^[a-f0-9]{32}$/.test(value.nonce) ||
        !Number.isSafeInteger(value.exp) || value.exp <= sec || value.exp > sec + TICKET_TTL_SEC ||
        !Number.isSafeInteger(value.sessionExpiresAt) || value.sessionExpiresAt <= now) return null;
    return value;
  } catch { return null; }
}

// Used at socket admission/renewal and on each write, never on device telemetry.
export async function livePermission(env, claims, write = false) {
  return env.DB.prepare(`SELECT ud.role, d.device_key_hash FROM user_sessions s JOIN users u ON u.google_sub=s.user_sub
    JOIN user_devices ud ON ud.user_sub=s.user_sub
    JOIN devices d ON d.device_id=ud.device_id
    LEFT JOIN device_inventory i ON i.device_id=ud.device_id
    WHERE s.id=? AND s.user_sub=? AND s.revoked_at IS NULL AND s.expires_at>?
      AND u.disabled=0 AND ud.device_id=? AND COALESCE(i.enabled,1)=1
      ${write ? "AND ud.role IN ('owner','operator')" : ''}`)
    .bind(claims.sessionId, claims.userSub, Date.now(), claims.deviceId).first();
}
export async function verifyWrite(env, id, clientId, channel, wire, bootId, now = Date.now()) {
  if (!WRITE_CHANNELS.has(channel) || wire?.v !== 2 || typeof wire.body !== 'string' ||
      encoder.encode(wire.body).length >= 1700 || typeof wire.grant !== 'string') return null;
  const parts = wire.grant.split('|');
  const sec = Math.floor(now / 1000), expiry = Number(parts[1]);
  if (parts.length !== 3 || parts[0] !== clientId || !CLIENT_ID.test(parts[0]) ||
      !/^[0-9]+$/.test(parts[1]) || !/^[a-f0-9]{24}$/.test(parts[2]) ||
      !Number.isSafeInteger(expiry) || expiry < sec || expiry > sec + 300 ||
      !/^[a-f0-9]{64}$/.test(wire.grantSig || '') || !/^[a-f0-9]{64}$/.test(wire.sig || '')) return null;
  const command = await hmacHex(env.DEVICE_KEY_PEPPER, 'mayap-command-key:v1:' + id);
  const key = Uint8Array.from(command.match(/../g), v => parseInt(v, 16));
  if (!timingSafeEqual(wire.grantSig, await hmacHex(key, `mayap-control-grant:v2\n${id}\n${wire.grant}`))) return null;
  const sessionKey = await hmacHex(key, `mayap-control-session:v2\n${id}\n${wire.grant}`);
  const sessionBytes = Uint8Array.from(sessionKey.match(/../g), v => parseInt(v, 16));
  if (!timingSafeEqual(wire.sig, await hmacHex(sessionBytes, `mayap-mqtt-write:v2\n${id}\n${channel}\n${wire.grant}\n${wire.body}`))) return null;
  try {
    const body = JSON.parse(wire.body);
    if (body.v !== 2 || body.clientId !== clientId || body.bootId !== bootId ||
        typeof body.requestId !== 'string' || !/^[A-Za-z0-9_-]{1,39}$/.test(body.requestId) ||
        !Number.isSafeInteger(body.seq) || body.seq <= 0 || !/^[a-f0-9]{16,64}$/.test(body.nonce || '')) return null;
    if ((channel === 'command' || channel === 'notes/request') && (!Number.isSafeInteger(body.expiresAt) || body.expiresAt < sec || body.expiresAt > sec + 30)) return null;
    return body;
  } catch { return null; }
}
