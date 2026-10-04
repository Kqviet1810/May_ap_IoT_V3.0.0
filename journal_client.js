/* Shared notes/reminders journal client. Only signed device replies are input. */
(function(root){
  'use strict';
  const u32=n=>Number.isInteger(n)&&n>=0&&n<=0xffffffff;
  const validNote=n=>n&&/^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/.test(n.id)&&
    u32(n.version)&&n.version>0&&Number.isSafeInteger(n.createdAt)&&n.createdAt>0&&
    ['batch','machine'].includes(n.type)&&typeof n.title==='string'&&n.title.length<=60&&
    typeof n.content==='string'&&n.content.length<=300&&n.content.trim().length>0;
  function create(exchange,options={}){
    const states=new Map(),locks=new Set();
    const state=id=>{if(!states.has(id))states.set(id,{generation:null,versions:new Map()});return states.get(id);};
    async function exclusive(id,fn){if(locks.has(id))throw new Error('Máy đang xử lý yêu cầu lưu khác. Thử lại sau.');
      locks.add(id);try{return await fn(state(id));}finally{locks.delete(id);}}
    async function ask(id,action,body){const result=await exchange(id,action,body);
      if(!result||!u32(result.generation))throw new Error('Phản hồi journal không hợp lệ.');return result;}
    async function load(id,s){
      const notes=[],seen=new Set();let cursor=0,generation=null;
      for(let page=0;page<=24;page++){
        const r=await ask(id,'notes.list',{cursor,...(generation===null?{}:{generation})});
        if(generation!==null&&r.generation!==generation)throw new Error('Dữ liệu máy vừa thay đổi. Bấm Làm mới.');
        generation=r.generation;
        if(r.note){if(!validNote(r.note)||seen.has(r.note.id))throw new Error('Dữ liệu Ghi chú không hợp lệ.');seen.add(r.note.id);notes.push(r.note);}
        if(typeof r.done!=='boolean'||!u32(r.next)||r.next>24)throw new Error('Trang journal không hợp lệ.');
        if(r.done){if(notes.length>16)throw new Error('Số ghi chú vượt giới hạn journal.');
          s.generation=generation;s.versions=new Map(notes.map(n=>[n.id,n.version]));return notes;}
        if(r.next<=cursor)throw new Error('Cursor journal không tiến lên.');cursor=r.next;
      }throw new Error('Journal vượt giới hạn số trang.');
    }
    // Browser persistence is an unresolved-operation guard, never note storage.
    // Only identity, revision and SHA-256 of intent survive reload; no key,
    // signature, note text or unsigned device data is persisted here.
    const unresolved=new Map();
    const key=id=>`${options.prefix?.()||'mayap'}.journal.unresolved.v1.${id}`;
    function intent(id){
      if(unresolved.has(id))return unresolved.get(id);
      const text=options.storage?.getItem(key(id));if(!text)return null;
      let item;try{item=JSON.parse(text);}catch{throw new Error('Không đọc được giao dịch chưa đối soát. Không gửi lệnh ghi mới.');}
      if(!item||!['notes.save','notes.delete','notes.reminders.save'].includes(item.action)||!u32(item.generation)||
         !u32(item.version)||!/^[a-f0-9]{64}$/.test(item.digest))throw new Error('Giao dịch chưa đối soát không hợp lệ. Không gửi lệnh ghi mới.');
      unresolved.set(id,item);return item;
    }
    function remember(id,item){options.storage?.setItem(key(id),JSON.stringify(item));unresolved.set(id,item);}
    function forget(id){options.storage?.removeItem(key(id));unresolved.delete(id);}
    async function digest(action,body){
      const value=action==='notes.save'?[body.note.id,body.note.title,body.note.content,body.note.type]:
        action==='notes.delete'?[body.note.id]:body.reminders.map(r=>[r.day,r.label]);
      const hash=await root.crypto.subtle.digest('SHA-256',new TextEncoder().encode(JSON.stringify(value)));
      return [...new Uint8Array(hash)].map(b=>b.toString(16).padStart(2,'0')).join('');
    }
    async function reconcile(id,s,item,knownNotes){
      let result,match=false;
      if(item.action==='notes.reminders.save'){
        result=await ask(id,'notes.reminders.read',{});
        if(!Array.isArray(result.reminders)||!u32(result.version))throw new Error('Phản hồi Nhắc nhở không hợp lệ.');
        match=result.generation>=item.generation&&result.version>=item.version&&
          await digest(item.action,result)===item.digest;
      }else{
        const notes=knownNotes||await load(id,s),note=notes.find(n=>n.id===item.noteId);
        result={generation:s.generation,...(note?{note}:{})};
        match=s.generation>=item.generation&&(item.action==='notes.delete'?!note:
          note&&note.version>=item.version&&await digest(item.action,{note})===item.digest);
      }
      if(!match&&u32(item.completedGeneration)&&result.generation>=item.completedGeneration){
        // A verified terminal failure ends the original writer. A later signed
        // snapshot proves the desired state is absent/different; stop here and
        // require an explicit refreshed edit rather than silently writing again.
        forget(id);throw Object.assign(new Error('Đã đối soát kết quả lỗi trên máy. Bấm Làm mới trước khi thử lưu lại.'),
          {code:item.resultCode||'NOTE_JOURNAL_IO',definitive:true});
      }
      if(!match)throw Object.assign(new Error('Chưa đối soát được giao dịch trước trên máy. Không gửi lệnh ghi mới; bấm Làm mới để kiểm tra.'),
        {code:'JOURNAL_UNRESOLVED',stage:'RECONCILIATION_MISMATCH'});
      forget(id);s.generation=result.generation;return result;
    }
    async function mutate(id,s,action,body){
      const fingerprint=await digest(action,body),previous=intent(id);
      if(previous){const recovered=await reconcile(id,s,previous);
        if(previous.action===action&&previous.digest===fingerprint)return recovered;
        // Reconciliation changed the snapshot; caller must refresh before a
        // different mutation instead of publishing against its stale revision.
        throw Object.assign(new Error('Đã đối soát giao dịch trước. Bấm Làm mới trước khi lưu thay đổi khác.'),{code:'NOTE_JOURNAL_CONFLICT'});
      }
      const item={action,generation:body.generation,version:body.note?.version??body.version??0,
        noteId:body.note?.id||'',digest:fingerprint};
      remember(id,item); // durable guard BEFORE a mutation can be published
      try{const result=await ask(id,action,body);
        if(action==='notes.save'&&(!validNote(result.note)||await digest(action,result)!==fingerprint))
          throw new Error('Máy chưa trả đúng nội dung đã lưu. Bấm Làm mới để kiểm tra.');
        if(action==='notes.reminders.save'&&(!Array.isArray(result.reminders)||await digest(action,result)!==fingerprint))
          throw new Error('Máy chưa trả đúng Nhắc nhở đã lưu.');
        forget(id);return result;}
      catch(error){
        if(error.definitive===true){forget(id);throw error;}
        if(error.deviceCompleted===true&&u32(error.revision)&&['NOTE_JOURNAL_IO','NOTE_JOURNAL_CORRUPT'].includes(error.code)){
          item.completedGeneration=error.revision;item.resultCode=error.code;remember(id,item);
        }
        // DATA + terminal ACK of a fresh generation-fenced READ are required.
        // Never create/re-sign another mutation, including after page reload.
        if(['UNCERTAIN','JOURNAL_DATA_MISSING','JOURNAL_TERMINAL_ACK_MISSING','JOURNAL_COMPLETION_MISSING','JOURNAL_DEVICE_RECEIPT_MISSING'].includes(error.code))return reconcile(id,s,item);
        throw error;
      }
    }
    return {
      reset(id){states.delete(id);},
      list:id=>exclusive(id,async s=>{const notes=await load(id,s),pending=intent(id);
        if(pending)await reconcile(id,s,pending,notes);return notes;}),
      save:(id,note)=>exclusive(id,async s=>{
        if(s.generation===null)await load(id,s);
        const candidate={...note,version:s.versions.get(note.id)||0};
        const r=await mutate(id,s,'notes.save',{generation:s.generation,note:candidate});
        if(!validNote(r.note)||r.note.id!==note.id||r.note.title!==note.title||r.note.content!==note.content||r.note.type!==note.type)
          throw new Error('Máy chưa trả đúng nội dung đã lưu. Bấm Làm mới để kiểm tra.');
        s.generation=r.generation;s.versions.set(r.note.id,r.note.version);return r.note;
      }),
      remove:(id,noteId)=>exclusive(id,async s=>{
        if(s.generation===null)await load(id,s);
        const r=await mutate(id,s,'notes.delete',{generation:s.generation,note:{id:noteId,version:s.versions.get(noteId)||0}});
        s.generation=r.generation;s.versions.delete(noteId);
      }),
      readReminders:id=>exclusive(id,async s=>{
        const r=await ask(id,'notes.reminders.read',{});if(!Array.isArray(r.reminders)||r.reminders.length>10||!u32(r.version))throw new Error('Nhắc nhở trong journal không hợp lệ.');
        s.generation=r.generation;return r;
      }),
      saveReminders:(id,reminders)=>exclusive(id,async s=>{
        const current=await ask(id,'notes.reminders.read',{});
        if(!u32(current.version))throw new Error('Revision Nhắc nhở không hợp lệ.');
        const r=await mutate(id,s,'notes.reminders.save',{generation:current.generation,version:current.version,reminders});
        if(!Array.isArray(r.reminders)||JSON.stringify(r.reminders)!==JSON.stringify(reminders))throw new Error('Máy chưa trả đúng Nhắc nhở đã lưu.');
        s.generation=r.generation;return r;
      })
    };
  }
  root.MayapJournalClient={create,validNote};
  if(typeof module==='object')module.exports=root.MayapJournalClient;
})(typeof window==='object'?window:globalThis);
