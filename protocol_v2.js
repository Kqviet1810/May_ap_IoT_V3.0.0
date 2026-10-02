// Pure transaction state machine shared by the UI and Node regression tests.
(function (root) {
  'use strict';
  const PacketPolicy = Object.freeze({ HARD_CAP: 4096, NORMAL_CAP: 2048,
    SMALL_TARGET: 512, CHUNK_TARGET: 1024, MQTT_OVERHEAD: 5,
    UNCERTAIN_TTL_MS: 120000 });
  class TransactionLedger {
    constructor(clock = () => performance.now()) {
      this.clock = clock;
      this.entries = new Map();
    }
    create(id, operation) {
      if (this.entries.has(id)) throw new Error('Duplicate requestId');
      const tx = { operation, phase: 'CREATED', tCreated: this.clock() };
      this.entries.set(id, tx);
      return tx;
    }
    published(id) {
      const tx = this.entries.get(id);
      if (tx && tx.phase === 'CREATED') {
        tx.phase = 'PUBLISHED'; tx.tPublished = this.clock();
      }
      return tx;
    }
    ack(id, ack) {
      const tx = this.entries.get(id);
      if (!tx || ['APPLIED', 'REJECTED'].includes(tx.phase)) return 'IGNORED';
      if (ack?.v !== 2 || ack.requestId !== id || ack.operation !== tx.operation ||
          !['received', 'completed', 'uncertain'].includes(ack.phase) ||
          typeof ack.ok !== 'boolean' || typeof ack.code !== 'string') return 'PROTOCOL_ERROR';
      if (ack.phase === 'received') {
        if (tx.phase !== 'UNCERTAIN') {
          tx.phase = 'RECEIVED'; tx.tDeviceReceived = this.clock();
        }
        return tx.phase;
      }
      if (ack.phase === 'uncertain') {
        return this.uncertain(id);
      }
      tx.phase = ack.ok ? 'APPLIED' : 'REJECTED';
      tx.code = ack.code;
      tx.tDeviceCompleted = this.clock();
      return tx.phase;
    }
    timeout(id) {
      return this.uncertain(id);
    }
    expireUncertain(now = this.clock(), ttl = PacketPolicy.UNCERTAIN_TTL_MS) {
      const expired = [];
      for (const [id, tx] of this.entries) {
        if (tx.phase === 'UNCERTAIN' && now - tx.tUncertain >= ttl) {
          expired.push(id);
          this.entries.delete(id);
        }
      }
      return expired;
    }
    // Retain the entry and its operation until a late signed terminal ACK or expiry.
    uncertain(id) {
      const tx = this.entries.get(id);
      if (tx && !['APPLIED', 'REJECTED'].includes(tx.phase)) {
        tx.phase = 'UNCERTAIN';
        tx.tUncertain ??= this.clock();
      }
      return tx?.phase;
    }
    remove(id) { this.entries.delete(id); }
  }
  const api = { TransactionLedger, PacketPolicy };
  if (typeof module === 'object' && module.exports) module.exports = api;
  root.MayapProtocolV2 = api;
})(typeof window !== 'undefined' ? window : globalThis);
