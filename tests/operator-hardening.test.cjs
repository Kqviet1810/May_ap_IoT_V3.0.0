const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const read = (name) => fs.readFileSync(path.resolve(__dirname, '..', name), 'utf8');
const dir = 'MAYAP_INDUSTRIAL_v1_0_0/';
const config = read(dir + 'config.h');
const ino = read(dir + 'MAYAP_INDUSTRIAL_v1_0_0.ino');
const network = read(dir + 'network_service.h');
const machine = read(dir + 'machine_control.h');
const hmi = read(dir + 'hmi.h');
const html = read('index.html');
const app = read('app.js');

test('boot home release is sensor-resolved and independent of Wi-Fi', () => {
  assert.match(machine, /bool startupResolved\(\) const \{ return startupResolved_; \}/);
  assert.match(machine, /bool sensorStartupResolved\(\) const \{ return sensor_\.startupResolved\(\); \}/);
  assert.match(ino, /sensorStartupResolved/);
  assert.match(ino, /sensorResolved[\s\S]{0,300}localTaskStability\.held\(now, homeDelay\)/);
  assert.doesNotMatch(hmi, /mayapBootHomeReleased\(\) \|\|/);
});

test('E405 is a local non-sounding reminder with periodic HMI presentation', () => {
  assert.match(machine, /WifiDisconnected = 405/);
  assert.match(machine, /FaultCode::WifiDisconnected, FaultSeverity::Info, 20U, AlarmNone, false, false, false, false, false, false/);
  assert.match(machine, /status\.requestedMode == ConnectivityMode::Online[\s\S]{0,300}!status\.connected[\s\S]{0,250}WifiDisconnected/);
  assert.match(hmi, /case 405: return AlarmNone/);
  assert.match(hmi, /case 405: return "MAT KET NOI WIFI"/);
  assert.match(hmi, /600000UL/);
  assert.match(hmi, /showToast\("E405 MAT WIFI"/);
  assert.doesNotMatch(machine, /FaultCode::WifiDisconnected[^\n]*AlarmSystem/);
});

test('connection info uses owner-published SSID and signal line for loss', () => {
  assert.match(config, /char ssid\[33\]/);
  assert.match(config, /char networkSsid\[33\]/);
  assert.match(network, /publishedSsid/);
  assert.match(network, /snprintf\(status\.ssid, sizeof\(status\.ssid\), "%s", publishedSsid\)/);
  assert.match(machine, /runtime_\.networkSsid/);
  assert.match(hmi, /"WIFI: %s", currentRuntime\.networkSsid/);
  assert.match(hmi, /"SONG: MAT KET NOI"/);
  assert.doesNotMatch(hmi, /WIFI: DA KET NOI/);
});

test('logged-in device page introduces adaptive thermal balance without changing its control path', () => {
  assert.match(html, /id="page-device"[\s\S]*id="thermalBalanceFeature"/);
  assert.match(html, /Cân bằng nhiệt thông minh/);
  assert.match(app, /thermalBalanceFeature/);
  assert.match(app, /adaptiveNames/);
  assert.match(app, /bảo vệ nhiệt vẫn độc lập/);
});
