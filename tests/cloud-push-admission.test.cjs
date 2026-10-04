const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');

const guard=fs.readFileSync('MAYAP_INDUSTRIAL_v1_0_0/network_io_guard.h','utf8');
const config=fs.readFileSync('MAYAP_INDUSTRIAL_v1_0_0/config.h','utf8');
const cloud=fs.readFileSync('MAYAP_INDUSTRIAL_v1_0_0/cloud_alert_link.h','utf8');

function constant(source,name) {
  const match=source.match(new RegExp('constexpr\\s+uint32_t\\s+'+name+'\\s*=\\s*(\\d+)U'));
  assert.ok(match, 'missing '+name);
  return Number(match[1]);
}
function byteConstant(source,name) {
  const match=source.match(new RegExp('constexpr\\s+uint8_t\\s+'+name+'\\s*=\\s*(\\d+)U'));
  assert.ok(match, 'missing '+name);
  return Number(match[1]);
}

test('Cloud TLS has its own bounded admission floor instead of inheriting OTA reserve',()=>{
  const mqtt=constant(guard,'MQTT_TLS_MIN_FREE_HEAP');
  const cloudFloor=constant(guard,'CLOUD_TLS_MIN_FREE_HEAP');
  const ota=constant(guard,'OTA_TLS_MIN_FREE_HEAP');
  assert.equal(mqtt,49152);
  assert.equal(cloudFloor,65536);
  assert.equal(ota,73728);
  assert.ok(cloudFloor<ota);
  assert.match(guard,/case MayapTlsKind::Cloud: return CLOUD_TLS_MIN_FREE_HEAP/);
  assert.match(guard,/ESP\.getMaxAllocHeap\(\) < MayapNetworkIoInternal::TLS_MIN_LARGEST_BLOCK/);
});

test('Cloud alarm queue covers tracked faults and drains before routine heartbeat',()=>{
  assert.ok(byteConstant(config,'CLOUD_OUTBOX_SIZE')>=byteConstant(config,'CLOUD_ACTIVE_TRACK_SIZE'));
  const start=cloud.indexOf('serviceRegister(now);');
  assert.notEqual(start,-1);
  const tail=cloud.slice(start,start+700);
  assert.ok(tail.indexOf('drainOutbox(now);')>=0);
  assert.ok(tail.indexOf('serviceHeartbeat(now);')>=0);
  assert.ok(tail.indexOf('drainOutbox(now);')<tail.indexOf('serviceHeartbeat(now);'));
});
