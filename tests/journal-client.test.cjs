const test=require('node:test'),assert=require('node:assert/strict');
const {create}=require('../journal_client.js');
const note={id:'11111111-1111-4111-8111-111111111111',version:1,createdAt:1750000000000,type:'machine',title:'Soi trứng',content:'Ghi chú tiếng Việt'};
test('journal client paginates one stable snapshot and preserves durable note version',async()=>{
 const sent=[];const client=create(async(id,action,body)=>{sent.push({id,action,body});
  if(action==='notes.list')return body.cursor===0?{generation:5,next:1,done:false,note}:{generation:5,next:24,done:true};
  return {generation:6,note:{...body.note,version:6}};});
 assert.equal((await client.list('A'))[0].version,1);await client.save('A',{...note,content:'Sửa mới'});
 assert.equal(sent[1].body.generation,5);assert.equal(sent[2].body.note.version,1);assert.equal(sent[2].body.generation,5);
});
test('journal rejects changed snapshots, duplicate IDs and non-advancing cursor',async()=>{
 for(const mode of ['changed','duplicate','cursor']){
  const client=create(async(_,__,b)=>b.cursor===0?{generation:1,next:1,done:false,note}:
    mode==='changed'?{generation:2,next:24,done:true}:mode==='duplicate'?{generation:1,next:24,done:true,note}:{generation:1,next:1,done:false});
  await assert.rejects(client.list('A'));
 }
});
test('reminders share generation with notes and refresh before replacing list',async()=>{
 const sent=[],list=[{day:7,label:'Soi trứng ngày 7'}];
 const client=create(async(_,action,b)=>{sent.push({action,b});
  if(action==='notes.list')return {generation:1,next:24,done:true,note};
  if(action==='notes.reminders.read')return {generation:8,version:3,reminders:[]};
  return {generation:9,version:9,reminders:list};});
 await client.list('A');await client.saveReminders('A',list);
 assert.equal(sent[2].b.generation,8);assert.equal(sent[2].b.version,3);
});
test('per-device journal concurrency is bounded and failures retain retry eligibility',async()=>{
 let release;const client=create(()=>new Promise(r=>release=r));const first=client.list('A');
 await assert.rejects(client.list('A'),/đang xử lý/);release({generation:0,next:24,done:true});await first;
 const retry=client.list('A');release({generation:0,next:24,done:true});await retry;
});
test('journal never accepts an altered stored echo as successful save',async()=>{
 const client=create(async(_,action,b)=>action==='notes.list'?{generation:0,next:24,done:true}:
  {generation:1,note:{...b.note,version:1,content:'Không phải nội dung gửi'}});
 await assert.rejects(client.save('A',note),/đúng nội dung/);
});
