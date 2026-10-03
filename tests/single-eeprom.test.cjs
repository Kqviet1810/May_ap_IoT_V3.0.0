const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
test('notes and reminders share journal; old save route removed; runtime uses verified mailbox only',()=>{
 const control=fs.readFileSync('MAYAP_INDUSTRIAL_v1_0_0/machine_control.h','utf8');
 const realtime=fs.readFileSync('MAYAP_INDUSTRIAL_v1_0_0/realtime_link.h','utf8');
 assert.doesNotMatch(control,/store_\.(saveReminders|loadReminders)/);
 assert.doesNotMatch(realtime,/handleReminderSetMessage|pendingReminderSave|mayapNotesStart/);
 assert.match(control,/MayapNoteMailbox::takeReminders\(verifiedReminders,journalRevision\)/);
 assert.match(control,/reminders_=verifiedReminders;[\s\S]*MayapNoteMailbox::applied\(journalRevision\)/);
 const link=fs.readFileSync('MAYAP_INDUSTRIAL_v1_0_0/note_realtime.h','utf8');
 assert.match(link,/appliedRevision\(\)!=noteResult.generation/);
 assert.match(link,/publishAck\(notePending.id,ok\?"applied":"rejected"/);
});
