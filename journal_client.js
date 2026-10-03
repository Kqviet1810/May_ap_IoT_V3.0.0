/* Shared notes/reminders journal client. Only signed device replies are input. */
(function(root){
  'use strict';
  const u32=n=>Number.isInteger(n)&&n>=0&&n<=0xffffffff;
  const validNote=n=>n&&/^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/.test(n.id)&&
    u32(n.version)&&n.version>0&&Number.isSafeInteger(n.createdAt)&&n.createdAt>0&&
    ['batch','machine'].includes(n.type)&&typeof n.title==='string'&&n.title.length<=60&&
    typeof n.content==='string'&&n.content.length<=300&&n.content.trim().length>0;
  function create(exchange){
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
    return {
      reset(id){states.delete(id);},
      list:id=>exclusive(id,s=>load(id,s)),
      save:(id,note)=>exclusive(id,async s=>{
        if(s.generation===null)await load(id,s);
        const candidate={...note,version:s.versions.get(note.id)||0};
        const r=await ask(id,'notes.save',{generation:s.generation,note:candidate});
        if(!validNote(r.note)||r.note.id!==note.id||r.note.title!==note.title||r.note.content!==note.content||r.note.type!==note.type)
          throw new Error('Máy chưa trả đúng nội dung đã lưu. Bấm Làm mới để kiểm tra.');
        s.generation=r.generation;s.versions.set(r.note.id,r.note.version);return r.note;
      }),
      remove:(id,noteId)=>exclusive(id,async s=>{
        if(s.generation===null)await load(id,s);
        const r=await ask(id,'notes.delete',{generation:s.generation,note:{id:noteId,version:s.versions.get(noteId)||0}});
        s.generation=r.generation;s.versions.delete(noteId);
      }),
      readReminders:id=>exclusive(id,async s=>{
        const r=await ask(id,'notes.reminders.read',{});if(!Array.isArray(r.reminders)||r.reminders.length>10||!u32(r.version))throw new Error('Nhắc nhở trong journal không hợp lệ.');
        s.generation=r.generation;return r;
      }),
      saveReminders:(id,reminders)=>exclusive(id,async s=>{
        const current=await ask(id,'notes.reminders.read',{});
        if(!u32(current.version))throw new Error('Revision Nhắc nhở không hợp lệ.');
        const r=await ask(id,'notes.reminders.save',{generation:current.generation,version:current.version,reminders});
        if(!Array.isArray(r.reminders)||JSON.stringify(r.reminders)!==JSON.stringify(reminders))throw new Error('Máy chưa trả đúng Nhắc nhở đã lưu.');
        s.generation=r.generation;return r;
      })
    };
  }
  root.MayapJournalClient={create,validNote};
  if(typeof module==='object')module.exports=root.MayapJournalClient;
})(typeof window==='object'?window:globalThis);
