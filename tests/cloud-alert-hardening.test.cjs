const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');

const guard=fs.readFileSync('MAYAP_INDUSTRIAL_v1_0_0/network_io_guard.h','utf8');
const cloud=fs.readFileSync('MAYAP_INDUSTRIAL_v1_0_0/cloud_alert_link.h','utf8');

test('Cloud safety alerts have a smaller TLS admission floor than OTA',()=>{
  assert.match(guard,/MQTT_MIN_FREE_HEAP\s*=\s*49152U/);
  assert.match(guard,/CLOUD_MIN_FREE_HEAP\s*=\s*65536U/);
  assert.match(guard,/OTA_MIN_FREE_HEAP\s*=\s*73728U/);
  assert.match(guard,/TLS_MIN_LARGEST_BLOCK\s*=\s*24576U/);
  assert.match(guard,/MayapTlsDenyReason/);
  assert.match(guard,/case MayapTlsKind::Cloud: return MayapNetworkIoInternal::CLOUD_MIN_FREE_HEAP/);
  assert.match(guard,/case MayapTlsKind::Ota: return MayapNetworkIoInternal::OTA_MIN_FREE_HEAP/);
});

test('Cloud deferred admission is paced and is not treated as a network failure',()=>{
  assert.match(cloud,/CloudRequestResult\s*:\s*uint8_t\s*\{\s*Success,\s*Deferred,\s*Failed\s*\}/);
  assert.match(cloud,/CLOUD_DEFER_MEMORY_RETRY_MS\s*=\s*1000UL/);
  assert.match(cloud,/CLOUD_DEFER_BUSY_RETRY_MS\s*=\s*250UL/);
  assert.match(cloud,/return CloudRequestResult::Deferred/);
  assert.match(cloud,/\[CLOUD\] WAIT RAM/);
  assert.doesNotMatch(cloud,/requestDeferred/);
});

test('fault notifications are protected and cannot be marked queued until accepted',()=>{
  assert.match(cloud,/bool protectedEvent = false/);
  assert.match(cloud,/queueFaultActive\(FaultTrack &track/);
  assert.match(cloud,/if \(!enqueueLevel\(alarmType, levelForSeverity\(track\.severity\), body, true\)\) return false/);
  assert.match(cloud,/if \(!track\.activeQueued && !queueFaultActive\(track, now, false\)\) continue/);
  assert.match(cloud,/if \(queueFaultResolved\(track\)\) track = FaultTrack\{\}/);
  assert.doesNotMatch(cloud,/outboxCriticalDropped/);
});

test('pending alarms outrank heartbeat and use an independent backoff',()=>{
  assert.match(cloud,/static BackoffTimer routineBackoff/);
  assert.match(cloud,/static BackoffTimer alarmBackoff/);
  const scheduler=cloud.slice(cloud.indexOf('servicePinReset();'),cloud.indexOf('// MachineController goi ham nay'));
  assert.ok(scheduler.indexOf('drainOutbox(now);')>=0);
  assert.ok(scheduler.indexOf('serviceHeartbeat(now);')>=0);
  assert.ok(scheduler.indexOf('drainOutbox(now);')<scheduler.indexOf('serviceHeartbeat(now);'));
  assert.match(cloud,/if \(outboxCount > 0U\)/);
});

test('firmware reports Worker Push outcome after HTTP acceptance',()=>{
  assert.match(cloud,/notification_sent/);
  assert.match(cloud,/\[CLOUD-ALARM\].*HTTP=200 push=%d throttled=%u/);
});
