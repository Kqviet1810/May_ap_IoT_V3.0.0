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
function memory(){const data=new Map();return {getItem:k=>data.get(k)||null,setItem:(k,v)=>data.set(k,v),removeItem:k=>data.delete(k),data};}
const uncertain=()=>Object.assign(new Error('ACK/DATA missing'),{code:'UNCERTAIN'});
test('lost mutation ACK/DATA reconciles signed reads without a second save or delete',async()=>{
 for(const action of ['notes.save','notes.delete']){
  let notes=action==='notes.delete'?[note]:[],generation=1,mutations=0;
  const client=create(async(_,op,b)=>{
   if(op==='notes.list')return {generation,next:24,done:true,...(notes[0]?{note:notes[0]}:{})};
   mutations++;generation++;notes=op==='notes.save'?[{...b.note,version:generation}]:[];throw uncertain();
  });
  if(action==='notes.save')assert.equal((await client.save('A',note)).id,note.id);
  else await client.remove('A',note.id);
  assert.equal(mutations,1);
 }
});
test('reminder lost ACK reconciles exact list with a fresh read and no rewrite',async()=>{
 let generation=0,version=0,list=[],mutations=0;const reminders=[{day:7,label:'Soi trứng'}];
 const client=create(async(_,op,b)=>{
  if(op==='notes.reminders.read')return {generation,version,reminders:list};
  mutations++;list=b.reminders;version=++generation;throw uncertain();
 });
 assert.deepEqual((await client.saveReminders('A',reminders)).reminders,reminders);assert.equal(mutations,1);
});
test('reload retains guard and performs read reconciliation without persisting note text or signing material',async()=>{
 const storage=memory();let generation=0,stored=null,mutations=0,offline=false;
 const exchange=async(_,op,b)=>{
  if(op==='notes.list'){if(offline)throw uncertain();return {generation,next:24,done:true,...(stored?{note:stored}:{})};}
  mutations++;stored={...b.note,version:++generation};offline=true;throw uncertain();
 };
 await assert.rejects(create(exchange,{storage}).save('A',note));assert.equal(storage.data.size,1);
 const text=[...storage.data.values()][0];assert.ok(!text.includes(note.content)&&!text.includes(note.title));
 assert.ok(!text.includes('grant')&&!text.includes('sig'));
 offline=false;const reloaded=create(exchange,{storage});assert.equal((await reloaded.list('A'))[0].id,note.id);
 assert.equal(mutations,1);assert.equal(storage.data.size,0);
});
test('unresolved mismatch, older snapshot and corruption block any new mutation',async()=>{
 const storage=memory();let mutations=0,generation=3,stored=null;
 const exchange=async(_,op)=>op==='notes.list'?{generation,next:24,done:true,...(stored?{note:stored}:{})}:
  (mutations++,Promise.reject(uncertain()));
 const client=create(exchange,{storage});await assert.rejects(client.save('A',note),/đối soát/);
 for(const mode of ['different','older']){
  stored={...note,version:mode==='older'?1:4,content:mode==='older'?note.content:'Khác'};generation=mode==='older'?2:4;
  await assert.rejects(create(exchange,{storage}).save('A',{...note,content:'Mới'}));assert.equal(mutations,1);
 }
 storage.setItem('mayap.journal.unresolved.v1.A','not JSON');await assert.rejects(create(exchange,{storage}).save('A',note),/Không đọc/);assert.equal(mutations,1);
});
test('definitive Hub/EEPROM rejects retain real code and release guard without fake reconciliation',async()=>{
 for(const code of ['ACCESS_DENIED','DEVICE_OFFLINE','BUSY','NOTE_JOURNAL_CONFLICT','NOTE_JOURNAL_FULL']){
  const storage=memory();let reads=0;
  const client=create(async(_,op)=>{if(op==='notes.list'){reads++;return {generation:0,next:24,done:true};}
   throw Object.assign(new Error(code),{code,definitive:true});},{storage});
  await assert.rejects(client.save('A',note),e=>e.code===code);assert.equal(storage.data.size,0);assert.equal(reads,1);
 }
});
test('invalid signature never confirms a mutation; guard remains until verified read',async()=>{
 for(const code of ['DATA_SIGNATURE_INVALID','ACK_SIGNATURE_INVALID']){
  const storage=memory();let reads=0;
  const client=create(async(_,op)=>{if(op==='notes.list'){reads++;return {generation:0,next:24,done:true};}
   throw Object.assign(new Error(code),{code});},{storage});
  await assert.rejects(client.save('A',note),e=>e.code===code);assert.equal(reads,1);assert.equal(storage.data.size,1);
 }
});

test('I/O terminal failure keeps guard until a fresh authenticated snapshot establishes the result',async()=>{
 for(const committed of [false,true]){
  const storage=memory();let generation=0,stored=null,writes=0;
  const exchange=async(_,op,b)=>{
   if(op==='notes.list')return {generation,next:24,done:true,...(stored?{note:stored}:{})};
   writes++;if(committed)stored={...b.note,version:++generation};
   throw Object.assign(new Error('NOTE_JOURNAL_IO'),{code:'NOTE_JOURNAL_IO',deviceCompleted:true,revision:generation});
  };
  const client=create(exchange,{storage});await assert.rejects(client.save('A',note),e=>e.code==='NOTE_JOURNAL_IO');
  assert.equal(storage.data.size,1);
  if(committed)assert.equal((await create(exchange,{storage}).list('A'))[0].id,note.id);
  else await assert.rejects(create(exchange,{storage}).list('A'),/Đã đối soát/);
  assert.equal(storage.data.size,0);assert.equal(writes,1);
 }
});

test('known forwarding/device stages retain specific codes and still reconcile with READ only',async()=>{
 for(const code of ['JOURNAL_DATA_MISSING','JOURNAL_TERMINAL_ACK_MISSING','JOURNAL_COMPLETION_MISSING','JOURNAL_DEVICE_RECEIPT_MISSING']){
  let stored=null,generation=0,writes=0;
  const client=create(async(_,op,b)=>{if(op==='notes.list')return {generation,next:24,done:true,...(stored?{note:stored}:{})};
   writes++;stored={...b.note,version:++generation};throw Object.assign(new Error(code),{code});});
  assert.equal((await client.save('A',note)).id,note.id);assert.equal(writes,1);
 }
});
