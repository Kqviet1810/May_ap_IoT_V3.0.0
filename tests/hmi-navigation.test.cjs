const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const hmi = fs.readFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v4_0_0', 'hmi.h'), 'utf8');
const config = fs.readFileSync(path.join(root, 'MAYAP_INDUSTRIAL_v4_0_0', 'config.h'), 'utf8');

function bodyOf(signature) {
  const start = hmi.indexOf(signature);
  assert.notEqual(start, -1, `${signature} is missing`);
  const open = hmi.indexOf('{', start);
  let depth = 0;
  for (let i = open; i < hmi.length; i++) {
    if (hmi[i] === '{') depth++;
    if (hmi[i] === '}' && --depth === 0) return hmi.slice(open + 1, i);
  }
  throw new Error(`Unclosed ${signature}`);
}

test('one handled hold cannot trigger a second screen action', () => {
  const reset = bodyOf('void resetRotaryPending(');
  const guard = bodyOf('void armInputGuard(');
  const input = bodyOf('void handleInput()');
  assert.match(reset, /rearmSwallowedLongPress\s*&&\s*rotary\.button == ButtonEvent::LongPress/);
  assert.match(guard, /resetRotaryPending\(\);/);
  assert.match(input, /if \(!timeReached\(now, inputGuardUntil\)\)\s*\{\s*resetRotaryPending\(true\);/);
});

test('home hold never falls through to batch start or menu', () => {
  const home = bodyOf('void activateHomeContext(bool longPress)');
  assert.match(home, /if \(longPress\)\s*\{[\s\S]*?return;\s*\}/);
  assert.match(home, /eventLogFaultsOnly = true/);
  assert.match(home, /openAlarmView\(View::Home\)/);
  assert.match(home, /openBatchConfirm\(View::Home\)/);
});

test('button timing has release debouncing and release-time hold fallback', () => {
  const button = bodyOf('void updateRotary(uint32_t now)');
  assert.match(config, /BUTTON_LONG_PRESS_MS = 700UL/);
  assert.match(config, /BUTTON_RELEASE_DEBOUNCE_MS = 70UL/);
  assert.match(button, /raw == HIGH \? BUTTON_RELEASE_DEBOUNCE_MS : BUTTON_DEBOUNCE_MS/);
  assert.match(button, /now - rotary\.pressedAt >= BUTTON_LONG_PRESS_MS \?\s*ButtonEvent::LongPress : ButtonEvent::ShortPress/);
});

test('all settings remain reachable exactly once in shorter groups', () => {
  const indexes = hmi.match(/const uint8_t GROUP_SETTING_INDEXES\[\] = \{([\s\S]*?)\};/)[1]
    .replace(/\/\/[^\n]*/g, '').match(/\d+/g).map(Number);
  const groups = [...hmi.matchAll(/\{"([^"\n]+)",\s*(\d+),\s*(\d+)\}/g)]
    .map((match) => ({ name: match[1], first: Number(match[2]), count: Number(match[3]) }));
  assert.equal(indexes.length, 33);
  assert.equal(new Set(indexes).size, 33);
  assert.deepEqual([...indexes].sort((a, b) => a - b), [
    ...Array.from({ length: 33 }, (_, i) => i).filter((i) => ![33, 34, 35, 36, 37, 38, 39, 40, 41].includes(i)),
  ]);
  assert.equal(groups.length, 8);
  assert.equal(groups.at(-1).name, 'TAO AM');
  let next = 0;
  for (const group of groups) {
    assert.equal(group.first, next);
    assert.ok(group.count <= 8, `${group.name} is too long`);
    next += group.count;
  }
  assert.equal(next, indexes.length);
  const vent = bodyOf('void drawVentilationMenu()');
  const advanced = bodyOf('void drawVentilationAdvanced()');
  assert.match(vent, /ventAutoEnabled/);
  assert.match(advanced, /SETTINGS\[44U \+ index\]/);
});
