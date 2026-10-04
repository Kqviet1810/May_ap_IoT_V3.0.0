const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');

const guard=fs.readFileSync('MAYAP_INDUSTRIAL_v1_0_0/network_io_guard.h','utf8');
const cloud=fs.readFileSync('MAYAP_INDUSTRIAL_v1_0_0/cloud_alert_link.h','utf8');
const config=fs.readFileSync('MAYAP_INDUSTRIAL_v1_0_0/config.h','utf8');

test('Cloud TLS admission is lower than OTA but keeps the contiguous-block safety floor',()=>{
  assert.match(guard,/TLS_FREE_MIN_MQTT\s*=\s*49152U/);
  assert.match(guard,/TLS_FREE_MIN_CLOUD\s*=\s*65536U/);
  assert.match(guard,/TLS_FREE_MIN_OTA\s*=\s*73728U/);
  assert.match(guard,/TLS_LARGEST_BLOCK_MIN\s*=\s*24576U/);
  assert.match(guard,/case MayapTlsKind::Cloud: return TLS_FREE_MIN_CLOUD/);
  assert.match(guard,/case MayapTlsKind::Ota: return TLS_FREE_MIN_OTA/);
  assert.match(guard,/MayapTlsDeferReason::FreeHeap/);
  assert.match(guard,/MayapTlsDeferReason::LargestBlock/);
});

test('Cloud admission deferral is paced instead of retrying every network loop',()=>{
  assert.match(config,/CLOUD_TLS_BUSY_RETRY_MS\s*=\s*250UL/);
  assert.match(config,/CLOUD_TLS_MEMORY_RETRY_MS\s*=\s*1000UL/);
  assert.match(cloud,/static uint32_t cloudAdmissionRetryAt = 0U/);
  assert.match(cloud,/deferCloudAdmission\(requestNow, tlsOperation\.deferReason\(\)\)/);
  for(const fn of ['drainOutbox','serviceHeartbeat','serviceRegister','servicePinReset']){
    const start=cloud.indexOf('inline void '+fn+'(');
    assert.notEqual(start,-1,fn+' missing');
    const body=cloud.slice(start,cloud.indexOf('\n}',start)+2);
    assert.match(body,/cloudAdmissionReady\(now\)/,fn+' must respect paced admission');
  }
});

test('Critical alarms outrank routine heartbeat and queued noncritical events',()=>{
  const update=cloud.slice(cloud.indexOf('inline void mayapCloudAlertUpdate'));
  const drain=update.indexOf('drainOutbox(now);');
  const heartbeat=update.indexOf('serviceHeartbeat(now);');
  assert.ok(drain>=0 && heartbeat>drain,'alarm drain must run before heartbeat');
  assert.match(update,/if \(outboxCount == 0U\) serviceHeartbeat\(now\);/);
  assert.match(cloud,/nextOutboxOffset\(\)[\s\S]*NotifyLevel::Critical/);
});

test('Queue pressure never evicts an older Critical event',()=>{
  assert.doesNotMatch(cloud,/outboxCriticalDropped/);
  assert.match(cloud,/if \(severity == NotifyLevel::Critical\)[\s\S]*\+\+outboxCriticalDeferred;[\s\S]*return false;/);
  assert.match(cloud,/p < incomingPriority && p < victimPriority/);
  assert.match(cloud,/events can evict Warning\/System\/Info, but NEVER another Critical event/);
});

test('Fault tracker only advances sent timestamps after the outbox accepts the alarm',()=>{
  const enqueue=cloud.slice(cloud.indexOf('inline bool enqueueFaultActive'),cloud.indexOf('inline void checkFaults'));
  assert.match(enqueue,/if \(!enqueueLevel\([^;]+\)\) return false;/);
  assert.match(enqueue,/track\.announced = true/);
  assert.match(enqueue,/track\.lastSentAt = now/);

  const faults=cloud.slice(cloud.indexOf('inline void checkFaults'),cloud.indexOf('// --------------------------- Su kien mot lan'));
  assert.match(faults,/if \(!track\.announced\)[\s\S]*enqueueFaultActive\(track, now, false\)/);
  assert.match(faults,/Critical fault appeared and recovered[\s\S]*if \(!enqueueFaultActive\(track, now, false\)\) continue;/);
  assert.match(faults,/if \(enqueueResolved\([^;]+\)\) \{[\s\S]*track = FaultTrack\{\};/);
});

test('Phase A firmware is versioned independently from the unchanged Web PWA',()=>{
  assert.match(config,/MAYAP_FIRMWARE_VERSION\[\]\s*=\s*"1\.1\.3"/);
  const manifest=JSON.parse(fs.readFileSync('release-manifest.json','utf8'));
  assert.equal(manifest.firmware,'1.1.3');
  assert.equal(manifest.release,'1.1.3');
  assert.equal(manifest.web,'1.1.6');
});
