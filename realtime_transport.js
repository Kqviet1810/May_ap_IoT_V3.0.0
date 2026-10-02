// Bounded native WebSocket transport. Controller ACKs belong to protocol_v2.js;
// a DeviceHub forwarding receipt never means an action was applied.
(function (root) {
  'use strict';
  const CAP = 2048, MAX_PENDING = 16, STALE_MS = 90000;
  class Client {
    constructor(options) {
      this.options = options; this.deviceId = options.deviceId;
      this.handlers = new Map(); this.pending = new Map(); this.connected = false;
      this.disconnecting = false; this.generation = 0; this.backoff = 1000;
      this.ticket = options.ticket; this.url = options.url; this.lastPacketAt = 0;
      this.timer = 0; this.probeTimer = 0; this.refreshing = false;
      this.socket = null; this.clock = options.clock || (() => Date.now());
      this.random = options.random || Math.random;
      this.setTimer = options.setTimeout || root.setTimeout.bind(root); this.clearTimer = options.clearTimeout || root.clearTimeout.bind(root);
      this.Socket = options.WebSocket || root.WebSocket;
      queueMicrotask(() => this.open());
    }
    on(name, handler) { const list = this.handlers.get(name) || []; list.push(handler); this.handlers.set(name, list); return this; }
    emit(name, ...args) { for (const handler of this.handlers.get(name) || []) handler(...args); }
    active() { return !this.disconnecting && (this.options.isActive?.() ?? true); }
    async open(refresh = false) {
      if (!this.active() || this.opening) return;
      this.opening = true;
      const epoch = ++this.generation;
      try {
        if (refresh) { const session = await this.options.refresh(); if (epoch !== this.generation || !this.active()) return;
          this.url = session.url; this.ticket = session.ticket; }
        const url = new URL(this.url);
        if (url.protocol !== 'wss:' && !(url.protocol === 'ws:' && ['127.0.0.1', 'localhost'].includes(url.hostname))) throw new Error('INVALID_REALTIME_URL');
        if (typeof this.ticket !== 'string' || this.ticket.length > 1400) throw new Error('INVALID_TICKET');
        const socket = new this.Socket(url.href, ['mayap.v1', 'ticket.' + this.ticket]); this.socket = socket;
        this.timer = this.setTimer(() => { if (epoch === this.generation && !this.connected) this.replace(); }, 15000);
        socket.addEventListener('message', event => { if (epoch === this.generation) this.message(event.data); });
        socket.addEventListener('close', () => { if (epoch === this.generation) this.closed(); });
        socket.addEventListener('error', () => { if (epoch === this.generation) this.emit('error', new Error('SOCKET_ERROR')); });
      } catch (error) { if (epoch === this.generation) { this.emit('error', error); this.closed(); } }
      finally { if (epoch === this.generation) this.opening = false; }
    }
    message(text) {
      if (typeof text !== 'string' || new TextEncoder().encode(text).length > CAP) { this.replace(); return; }
      let msg; try { msg = JSON.parse(text); } catch { this.replace(); return; }
      this.lastPacketAt = this.clock(); this.clearTimer(this.probeTimer); this.probeTimer = 0;
      this.emit('packetreceive');
      if (msg.kind === 'ready') {
        if (msg.deviceId !== this.deviceId) { this.end(); return; }
        this.clearTimer(this.timer); this.connected = true; this.backoff = 1000;
        this.renewAt = this.clock() + 240000; this.emit('connect'); this.tick();
      } else if (msg.kind === 'renewed') { this.refreshing = false; this.renewAt = this.clock() + 240000; }
      else if (msg.kind === 'forwarded' || msg.kind === 'error') {
        const pending = this.pending.get(msg.requestId);
        if (pending) { this.pending.delete(msg.requestId); this.clearTimer(pending.timer);
          pending.callback(msg.kind === 'error' ? Object.assign(new Error(msg.code), { code: 'UNCERTAIN' }) : null); }
        if (msg.kind === 'error') this.emit('error', new Error(msg.code));
      } else if (msg.v === 1 && typeof msg.channel === 'string' && msg.payload && typeof msg.payload === 'object') {
        if (Number.isSafeInteger(msg.deliveryId)) {
          try { this.socket.send(JSON.stringify({ kind: 'received', deliveryId: msg.deliveryId })); }
          catch { this.replace(); }
        }
        this.emit('message', { deviceId: this.deviceId, channel: msg.channel }, msg.payload, { cached: msg.cached === true });
      }
    }
    send(route, payload, callback) {
      if (!this.connected || this.socket?.readyState !== 1 || route.deviceId !== this.deviceId) throw new Error('TRANSPORT_ERROR');
      const text = JSON.stringify({ v: 1, channel: route.channel, payload });
      if (new TextEncoder().encode(text).length > CAP || this.socket.bufferedAmount > CAP * 4) throw new Error('FRAME_OR_BUFFER_LIMIT');
      let id;
      if (callback) {
        try { id = JSON.parse(payload.body).requestId; } catch { throw new Error('INVALID_WRITE'); }
        if (this.pending.size >= MAX_PENDING || this.pending.has(id)) throw new Error('PENDING_LIMIT');
        const timer = this.setTimer(() => { const p = this.pending.get(id); if (!p) return; this.pending.delete(id);
          p.callback(Object.assign(new Error('FORWARD_RECEIPT_TIMEOUT'), { code: 'UNCERTAIN' })); }, 8000);
        this.pending.set(id, { callback, timer });
      }
      try { this.socket.send(text); } catch (error) {
        const p = this.pending.get(id); if (p) { this.clearTimer(p.timer); this.pending.delete(id); }
        throw error;
      }
    }
    renew(ticket) {
      if (!this.connected || this.socket?.readyState !== 1 || ticket === this.ticket) return;
      this.ticket = ticket; this.refreshing = true; this.refreshAt = this.clock();
      this.socket.send(JSON.stringify({ kind: 'renew', ticket }));
    }
    tick() {
      this.clearTimer(this.timer);
      this.timer = this.setTimer(async () => {
        if (!this.connected || this.disconnecting) return;
        if (this.refreshing && this.clock() - this.refreshAt > 15000) { this.replace(); return; }
        if (this.clock() - this.lastPacketAt > STALE_MS) { this.probe(); }
        else try { this.socket.send('{"kind":"ping"}'); } catch { this.replace(); return; }
        if (this.active() && !this.refreshing && this.clock() >= this.renewAt) {
          this.refreshing = true; this.refreshAt = this.clock(); const epoch = this.generation;
          try { const session = await this.options.refresh(); if (epoch === this.generation) this.renew(session.ticket); }
          catch { if (epoch === this.generation) this.replace(); return; }
        }
        if (this.connected && !this.disconnecting) this.tick();
      }, 30000);
    }
    probe() {
      if (!this.connected || this.probeTimer) return;
      const at = this.clock(), epoch = this.generation;
      this.probeTimer = this.setTimer(() => {
        this.probeTimer = 0;
        if (epoch !== this.generation || !this.active()) return;
        // A timeout delivered after OS suspension does not prove socket death.
        if (this.clock() - at > 16000) { this.probe(); return; }
        this.replace();
      }, 8000);
      try { this.socket.send('{"kind":"ping"}'); } catch { this.replace(); }
    }
    resume() { if (this.connected) this.probe(); else if (!this.disconnecting && !this.socket) { this.clearTimer(this.timer); this.open(true); } }
    replace() { if (this.disconnecting) return; ++this.generation; const old = this.socket; this.socket = null;
      try { old?.close(4002, 'STALE_OR_RESET'); } catch {} this.closed(); }
    closed() {
      ++this.generation; this.opening = false; this.refreshing = false; this.refreshAt = 0;
      this.clearTimer(this.timer); this.clearTimer(this.probeTimer); this.probeTimer = 0;
      this.connected = false; this.socket = null;
      for (const p of this.pending.values()) { this.clearTimer(p.timer); p.callback(Object.assign(new Error('CONNECTION_LOST'), { code: 'UNCERTAIN' })); }
      this.pending.clear(); this.emit('close');
      if (!this.active()) return;
      const delay = this.backoff + Math.floor(this.random() * this.backoff / 4);
      this.backoff = Math.min(60000, this.backoff * 2); this.emit('reconnect');
      this.timer = this.setTimer(() => this.open(true), delay);
    }
    end() { this.disconnecting = true; ++this.generation; try { this.socket?.close(1000, 'PAGE_END'); } catch {} this.closed(); }
  }
  const api = { Client, CAP, MAX_PENDING, STALE_MS };
  root.MayapRealtime = api;
  if (typeof module === 'object' && module.exports) module.exports = api;
})(typeof window === 'undefined' ? globalThis : window);
