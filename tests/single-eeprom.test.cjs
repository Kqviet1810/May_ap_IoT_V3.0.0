const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const read = name => fs.readFileSync(path.join(__dirname, '..', 'MAYAP_INDUSTRIAL_v4_0_0', name), 'utf8');
test('notes bridge applies only EEPROM readback and reports the final revision', () => {
  const control = read('machine_control.h');
  const start = control.indexOf('if (hmiTakeSavedReminders(');
  const flow = control.slice(start, control.indexOf('mayapSerialPrintf(false, "[REMIND]', start));
  assert.match(flow, /store_\.saveReminders\(requestedReminders, readbackReminders\)/);
  assert.match(flow, /if \(remindersOk\) \{\s*reminders_ = readbackReminders;/);
  assert.match(flow, /mayapWebConfirmReminderSave\(reminderTransactionId, remindersOk,/);
  assert.match(control, /store_\.loadReminders\(reminders_\)/);
  const realtime = read('realtime_link.h');
  const confirm = realtime.slice(realtime.indexOf('inline void mayapWebConfirmReminderSave('),
                                realtime.indexOf('inline void mayapWebPushEventLog('));
  assert.match(confirm, /if \(ok && stored\) \{[\s\S]*knownReminders = \*stored;[\s\S]*webRemindersRevision[\s\S]*remindersDirty = true;/);
  assert.match(confirm, /ok \? "applied" : "rejected"/);
});
