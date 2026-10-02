import { randomToken } from './auth.js';
import { json } from './account-auth.js';
import { FRAME_CAP, DEVICE_CHANNELS, WRITE_CHANNELS, CONNECTION_LEASE_MS,
  livePermission, verifyTicket, verifyWrite } from './realtime-auth.js';

const MAX_BROWSERS = 8, DEVICE_STALE_MS = 210000;
const encoder = new TextEncoder();
const attachment = ws => ws.deserializeAttachment();

// No controller logic, timers or unbounded telemetry storage. Socket attachments
// are the authoritative connection state and are recovered after hibernation.
export class DeviceHub {
  constructor(ctx, env) {
    this.ctx = ctx; this.env = env;
    this.ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('{"kind":"ping"}', '{"kind":"pong"}'));
  }
  save(ws, value) {
    if (encoder.encode(JSON.stringify(value)).length > 4096) { ws.close(1009, 'ATTACHMENT_LIMIT'); return false; }
    ws.serializeAttachment(value); return true;
  }
  sockets(kind) { return this.ctx.getWebSockets().filter(ws => ws.readyState === 1 && attachment(ws)?.kind === kind && !attachment(ws)?.superseded); }
  device() { return this.sockets('device')[0]; }
  send(ws, value) {
    try {
      const text = JSON.stringify(value);
      if (encoder.encode(text).length > FRAME_CAP) { ws.close(1009, 'FRAME_LIMIT'); return false; }
      ws.send(text); return true;
    } catch { try { ws.close(1013, 'BACKPRESSURE'); } catch {} return false; }
  }
  event(ws, channel, payload, cached = false) {
    const a = attachment(ws), frame = { v: 1, channel, payload };
    if (cached) frame.cached = true;
    if (a.kind === 'browser') {
      // Explicit receive credit bounds platform/TCP buffers for slow browsers.
      if ((a.delivery - a.received) >= 16) { ws.close(1013, 'SLOW_CONSUMER'); return false; }
      frame.deliveryId = ++a.delivery; this.save(ws, a);
    }
    return this.send(ws, frame);
  }
  broadcast(channel, payload, cached = false) {
    for (const ws of this.sockets('browser')) {
      const a = attachment(ws);
      if (a.until > Date.now() && (a.watchUntil > Date.now() || ['presence', 'ack'].includes(channel)))
        this.event(ws, channel, payload, cached);
    }
  }
  reject(ws, code, requestId = '') {
    // Transport errors are deliberately NOT device ACKs or applied outcomes.
    this.send(ws, { kind: 'error', code, requestId });
    console.warn(JSON.stringify({ event: 'realtime.reject', code, deviceId: attachment(ws)?.deviceId }));
  }
  async consumeTicket(claims) {
    const now = Date.now();
    return this.ctx.storage.transaction(async txn => {
      const used = await txn.get('tickets') || {};
      for (const [nonce, expiry] of Object.entries(used)) if (expiry <= now) delete used[nonce];
      if (used[claims.nonce] || Object.keys(used).length >= 64) return false;
      used[claims.nonce] = claims.exp * 1000;
      await txn.put('tickets', used); return true;
    });
  }
  async schedule() {
    const deadlines = this.sockets('browser').map(ws => attachment(ws).until);
    const device = this.device();
    if (device) deadlines.push(attachment(device).lastAt + DEVICE_STALE_MS);
    if (!deadlines.length) { await this.ctx.storage.deleteAlarm(); return; }
    const due = Math.max(Date.now() + 1000, Math.min(...deadlines));
    const old = await this.ctx.storage.getAlarm();
    if (old === null || old > due) await this.ctx.storage.setAlarm(due);
  }
  async fetch(request) { return this.ctx.blockConcurrencyWhile(() => this.admit(request)); }
  async admit(request) {
    if (request.method === 'POST' && new URL(request.url).pathname === '/invalidate-device') {
      for (const ws of this.sockets('device')) { const a = attachment(ws); a.superseded = true; this.save(ws, a); ws.close(4003, 'CREDENTIAL_ROTATED'); }
      this.broadcast('presence', {online:false, proto:2}); await this.schedule();
      return json({success:true});
    }
    if (request.headers.get('Upgrade')?.toLowerCase() !== 'websocket') return json({ error: 'UPGRADE_REQUIRED' }, 426);
    // Only the authenticated outer Worker can call this binding. It constructs
    // a fresh request and never forwards a caller-supplied admission header.
    const claims = JSON.parse(request.headers.get('X-Mayap-Admission') || 'null');
    if (!claims || !['device', 'browser'].includes(claims.kind)) return json({ error: 'AUTH_REQUIRED' }, 401);
    if (claims.kind === 'browser') {
      const role = await livePermission(this.env, claims);
      if (!role || role.role !== claims.role || !await this.consumeTicket(claims)) return json({ error: 'ACCESS_DENIED' }, 403);
      const same = this.sockets('browser').find(ws => attachment(ws).clientId === claims.clientId);
      if (!same && this.sockets('browser').length >= MAX_BROWSERS) return json({ error: 'CLIENT_LIMIT' }, 429);
      if (same) { const a = attachment(same); a.superseded = true; this.save(same, a); same.close(4001, 'REPLACED'); }
    }
    const pair = new WebSocketPair(), [client, server] = Object.values(pair);
    this.ctx.acceptWebSocket(server);
    const now = Date.now();
    if (claims.kind === 'device') {
      for (const old of this.sockets('device')) {
        const a = attachment(old); a.superseded = true; this.save(old, a); old.close(4001, 'REPLACED');
      }
      this.save(server, { ...claims, generation: randomToken(8), lastAt: now });
      this.broadcast('presence', { online: true, bootId: claims.bootId, proto: 2 });
      // Restore viewers immediately after a router/device reconnect.
      for (const ws of this.sockets('browser')) {
        const a = attachment(ws);
        if (a.watchUntil > now) this.event(server, 'session', { clientId: a.clientId, active: true,
          ttlMs: Math.min(60000, a.watchUntil - now), sync: true, scope: 'runtime',
          config: !!(a.scopeFlags & 1), reminders: !!(a.scopeFlags & 2), log: !!(a.scopeFlags & 4) });
      }
    } else {
      this.save(server, { kind: 'browser', deviceId: claims.deviceId, clientId: claims.clientId,
        sessionId: claims.sessionId, userSub: claims.userSub, role: claims.role, delivery: 0, received: 0, until: Math.min(now + CONNECTION_LEASE_MS, claims.sessionExpiresAt),
        watchUntil: 0, lastSeq: 0, requests: [], rateAt: now, rateCount: 0 });
      this.send(server, { kind: 'ready', deviceId: claims.deviceId, leaseMs: CONNECTION_LEASE_MS });
      const device = this.device(), a = device && attachment(device);
      this.event(server, 'presence', { ...(a?.presence || {}), online: !!device && now - a.lastAt < DEVICE_STALE_MS, bootId: a?.bootId || 0, proto: 2 });
      if (a?.bootstrap) this.event(server, 'bootstrap', a.bootstrap, true);
    }
    await this.schedule();
    const headers = new Headers();
    if (claims.kind === 'browser') headers.set('Sec-WebSocket-Protocol', 'mayap.v1');
    return new Response(null, { status: 101, webSocket: client, headers });
  }
  async webSocketMessage(ws, text) { return this.ctx.blockConcurrencyWhile(() => this.message(ws, text)); }
  async message(ws, text) {
    let a = attachment(ws);
    if (!a || a.superseded) return;
    if (typeof text !== 'string' || encoder.encode(text).length > FRAME_CAP) { ws.close(1009, 'FRAME_LIMIT'); return; }
    let msg; try { msg = JSON.parse(text); } catch { ws.close(1007, 'INVALID_JSON'); return; }
    const now = Date.now();
    if (a.kind === 'device') {
      if (this.device() !== ws || msg.v !== 1 || !DEVICE_CHANNELS.has(msg.channel) ||
          !msg.payload || typeof msg.payload !== 'object' || Array.isArray(msg.payload)) return this.reject(ws, 'INVALID_CHANNEL');
      if (['presence', 'bootstrap', 'snapshot', 'ack', 'config/reported', 'reminders/reported', 'history/reported'].includes(msg.channel) && msg.payload.bootId !== a.bootId)
        return this.reject(ws, 'STALE_BOOT');
      a.lastAt = now;
      if (msg.channel === 'presence') a.presence = msg.payload;
      if (msg.channel === 'bootstrap') a.bootstrap = msg.payload;
      this.save(ws, a);
      this.broadcast(msg.channel, msg.payload);
      // The existing HTTPS heartbeat supplies sparse D1/Push control-plane data.
      return;
    }
    if (a.until <= now) { ws.close(4003, 'SESSION_EXPIRED'); return; }
    if (now - a.rateAt >= 10000) { a.rateAt = now; a.rateCount = 0; }
    if (++a.rateCount > 80) { ws.close(1008, 'RATE_LIMIT'); return; }
    this.save(ws, a);
    if (msg.kind === 'received') {
      if (Number.isSafeInteger(msg.deliveryId) && msg.deliveryId > a.received && msg.deliveryId <= a.delivery) {
        a.received = msg.deliveryId; this.save(ws, a);
      }
      return;
    }
    if (msg.kind === 'renew') {
      const claims = await verifyTicket(this.env, msg.ticket);
      if (!claims || claims.sessionId !== a.sessionId || claims.clientId !== a.clientId || claims.deviceId !== a.deviceId ||
          claims.role !== a.role || !await livePermission(this.env, claims) || !await this.consumeTicket(claims)) return this.reject(ws, 'ACCESS_DENIED');
      a = attachment(ws); a.until = Math.min(now + CONNECTION_LEASE_MS, claims.sessionExpiresAt); this.save(ws, a);
      this.send(ws, { kind: 'renewed', leaseMs: CONNECTION_LEASE_MS }); await this.schedule(); return;
    }
    if (msg.v !== 1 || !msg.payload || typeof msg.payload !== 'object' || Array.isArray(msg.payload)) return this.reject(ws, 'INVALID_FRAME');
    const device = this.device();
    if (msg.channel === 'session') {
      if (msg.payload.clientId !== a.clientId) return this.reject(ws, 'CLIENT_MISMATCH');
      const ttl = Math.min(60000, Math.max(1000, Number(msg.payload.ttlMs) || 1000));
      a.watchUntil = msg.payload.active === true && msg.payload.foreground !== false ? now + ttl : 0;
      a.scopeFlags = (msg.payload.config === true ? 1 : 0) | (msg.payload.reminders === true ? 2 : 0) | (msg.payload.log === true ? 4 : 0);
      this.save(ws, a);
      if (device) this.event(device, 'session', { clientId: a.clientId, active: a.watchUntil > now,
        ttlMs: ttl, sync: msg.payload.sync === true, scope: 'runtime', config: !!(a.scopeFlags & 1),
        reminders: !!(a.scopeFlags & 2), log: !!(a.scopeFlags & 4) });
      const d = device && attachment(device);
      if (a.watchUntil && d?.bootstrap) this.event(ws, 'bootstrap', d.bootstrap, true);
      return;
    }
    if (!WRITE_CHANNELS.has(msg.channel) || a.role === 'viewer') return this.reject(ws, 'ACCESS_DENIED');
    const permission = await livePermission(this.env, a, true);
    if (!permission) return this.reject(ws, 'ACCESS_DENIED');
    if (device && attachment(device).keyHash !== permission.device_key_hash) {
      device.close(4003, 'CREDENTIAL_ROTATED');
      return this.reject(ws, 'DEVICE_REAUTH_REQUIRED');
    }
    if (!device || now - attachment(device).lastAt >= DEVICE_STALE_MS) return this.reject(ws, 'DEVICE_OFFLINE');
    const body = await verifyWrite(this.env, a.deviceId, a.clientId, msg.channel, msg.payload, attachment(device).bootId);
    if (!body) return this.reject(ws, 'INVALID_SIGNATURE_OR_EXPIRY', typeof msg.payload.body === 'string' ? (() => { try { return JSON.parse(msg.payload.body).requestId; } catch { return ''; } })() : '');
    // Exact retries are permitted, different bodies for one requestId are not.
    // The ESP32 independently owns the final replay/terminal/in-flight fences.
    const known = a.requests.find(r => r.id === body.requestId);
    const fingerprint = msg.payload.sig;
    if (known ? known.sig !== fingerprint : body.seq <= a.lastSeq) return this.reject(ws, 'REPLAY', body.requestId);
    a = attachment(ws);
    if (!known) { a.lastSeq = body.seq; a.requests.push({ id: body.requestId, sig: fingerprint }); a.requests = a.requests.slice(-16); this.save(ws, a); }
    if (!this.event(device, msg.channel, msg.payload)) return this.reject(ws, 'DEVICE_SEND_FAILED', body.requestId);
    this.send(ws, { kind: 'forwarded', requestId: body.requestId });
  }
  async webSocketClose(ws, code) {
    const a = attachment(ws);
    if (!a || a.superseded) return;
    a.superseded = true; this.save(ws, a);
    if (a.kind === 'device') this.broadcast('presence', { online: false, bootId: a.bootId, proto: 2 });
    else { const device = this.device(); if (device) this.event(device, 'session', { clientId: a.clientId, active: false, ttlMs: 1000 }); }
    console.info(JSON.stringify({ event: 'realtime.close', kind: a.kind, deviceId: a.deviceId, code }));
    await this.schedule();
  }
  async webSocketError(ws) { try { ws.close(1011, 'SOCKET_ERROR'); } catch {} await this.webSocketClose(ws, 1011); }
  async alarm() {
    const now = Date.now();
    for (const ws of this.sockets('browser')) if (attachment(ws).until <= now) ws.close(4003, 'SESSION_EXPIRED');
    const device = this.device();
    if (device && now - attachment(device).lastAt >= DEVICE_STALE_MS) { device.close(4002, 'STALE'); this.broadcast('presence', { online: false, bootId: attachment(device).bootId, proto: 2 }); }
    await this.ctx.storage.deleteAlarm(); await this.schedule();
  }
}
