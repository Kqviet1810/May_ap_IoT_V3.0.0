const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const read = (name) => fs.readFileSync(path.resolve(__dirname, '..', name), 'utf8').replace(/\r\n/g, '\n');
const dir = 'MAYAP_INDUSTRIAL_v1_0_0/';
function body(source, signature) {
  const start = source.indexOf(signature);
  assert.notEqual(start, -1, signature);
  const open = signature.endsWith('{') ? start + signature.length - 1 : source.indexOf('{', start);
  let depth = 0;
  for (let i = open; i < source.length; ++i) {
    if (source[i] === '{') ++depth;
    if (source[i] === '}' && --depth === 0) return source.slice(open + 1, i);
  }
  throw new Error(`Unclosed ${signature}`);
}
test('runtime recovery preserves Adaptive Boot, local safety, schemas, protocol and Web transactions', () => {
  const manifest = JSON.parse(read('tests/runtime-preservation.json'));
  for (const entry of manifest.entries) {
    let source = read(entry.file);
    if (entry.signature) source = body(source, entry.signature);
    // Only explicitly reviewed user-facing copy can differ inside protected Web functions.
    for (const [current, baseline] of entry.copyReplacements || []) source = source.replace(current, baseline);
    // The only additions allowed inside these protected functions are health instrumentation.
    if (entry.filter === 'webBeat') source = source.replace(/^\s*mayapServiceBeat\(MayapRecovery::Service::Ota\);\n/gm, '');
    if (entry.filter === 'supervisor') {
      source = source.replace('ALLOW_RUNTIME_HEALTH_AUTO_RESTART &&\n        ', '');
      source = source.replace(/    MayapRecovery::Service failedService[\s\S]*?(?=    const esp_err_t result = esp_task_wdt_reset\(\);)/, '');
    }
    if (entry.filter === 'config') source = source.replace(/^void mayapI2cReport\(uint8_t address, bool ok\);\n|^uint32_t mayapI2cRecoveryEpoch\(\);\n/gm, '');
    assert.equal(crypto.createHash('sha256').update(source).digest('hex'), entry.sha256, entry.file + ' ' + (entry.signature || ''));
  }
});
test('all service tasks admit and beat themselves, including isolation paths', () => {
  const ino = read(dir + 'MAYAP_INDUSTRIAL_v1_0_0.ino');
  for (const [task, service] of [['networkTask','Network'], ['mqttTask','Mqtt'], ['cloudTask','Cloud'], ['otaTask','Ota']]) {
    const source = body(ino, `void ${task}(`);
    assert.match(source, new RegExp(`mayapServiceAdmit\\(MayapRecovery::Service::${service}\\)`));
    assert.match(source, new RegExp(`mayapServiceBeat\\(MayapRecovery::Service::${service}\\)`));
    assert.match(source, /mayapServiceRecoveryComplete/);
    assert.match(source, /mayapServiceIsolated/);
    for (const prefix of source.split(/\bcontinue;/).slice(0,-1)) assert.match(prefix, /mayapServiceBeat/);
  }
});
test('online service failure degrades without controller restart authority', () => {
  const source = body(read(dir + 'MAYAP_INDUSTRIAL_v1_0_0.ino'), 'void supervisorTask(');
  const start = source.indexOf('MayapRecovery::Service failedService');
  const end = source.indexOf('const esp_err_t result = esp_task_wdt_reset();', start);
  assert.notEqual(start, -1);
  assert.notEqual(end, -1);
  const runtime = source.slice(start, end);
  assert.match(runtime, /mayapServiceSupervisorUpdate\(now\);/);
  assert.doesNotMatch(runtime, /mayapLatchSystemTrip|vTaskSuspend\(controlTaskHandle\)|mayapSafeOutputsEarly|mayapRestart/);
  assert.match(read(dir + 'runtime_recovery_policy.h'), /Action : uint8_t \{ None, Reinit, Isolate, Degraded \}/);
  assert.doesNotMatch(read(dir + 'runtime_recovery_policy.h'), /Action::Restart/);
  assert.match(read(dir + 'service_recovery.h'), /LOCAL CONTROL CONTINUES, NO RESTART/);
  for (const file of ['i2c_supervisor.h', 'service_recovery.h', 'runtime_recovery_policy.h', 'network_service.h'])
    assert.doesNotMatch(read(dir + file), /\bmayapRestart\(/);
});
test('shared I2C recovery has one bus reset owner and never clears physical faults', () => {test('shared I2C recovery has one bus reset owner and never clears physical faults', () => {
  const hmi = read(dir + 'hmi.h');
  assert.doesNotMatch(hmi, /recoverI2cBusUnlocked|Wire\.end\(|Wire\.begin\(/);
  const bus = body(read(dir + 'i2c_supervisor.h'), 'inline void mayapI2cSupervisorUpdate(');
  assert.ok(bus.indexOf('mayapI2cLock(0U)') < bus.indexOf('Wire.end()'));
  assert.match(bus, /pulse < 9U/);
  assert.match(bus, /mayapI2cUnlock/);
  assert.doesNotMatch(bus, /clearRecovered|EEPROM\.write|mayapRestart/);
});
