// Isolated physical-device stand-in. Note storage belongs to Node, survives Web
// reloads, and never uses browser localStorage. ACKs use the fixture session HMAC.
const {setup}=require('./test_web_experience.cjs');
const script=`window.__notesWireFixture=async (client,route,wire,cb)=>{
  cb?.(); const body=JSON.parse(wire.body),operation=route.channel==='notes/request'?'notes.read':body.action==='save'?'notes.save':'notes.delete';
  const response=await window.__notesDeviceCall({deviceId:route.deviceId,operation,body});
  const emit=(channel,payload)=>client.emit('message',{deviceId:route.deviceId,channel},payload,{cached:false});
  if(response.rows){
    for(let i=0;i<Math.max(1,response.rows.length);i++){
      const done=i===response.rows.length-1 || !response.rows.length;
      emit('notes/reported',{v:1,bootId:123,requestId:body.requestId,cursor:i,nextCursor:done?16:i+1,done,capacity:16,notes:response.rows[i]?[response.rows[i]]:[]});
    }
  }
  const ack={v:2,requestId:body.requestId,operation,phase:'completed',ok:response.ok,code:response.code,bootId:123,revision:response.version||0,message:response.message||response.code};
  const key=await crypto.subtle.importKey('raw',new Uint8Array(32).fill(7),{name:'HMAC',hash:'SHA-256'},false,['sign']);
  ack.sig=Array.from(new Uint8Array(await crypto.subtle.sign('HMAC',key,new TextEncoder().encode(['mayap-mqtt-ack:v2',route.deviceId,ack.requestId,operation,ack.phase,ack.ok?'1':'0',ack.code,ack.bootId,ack.revision,ack.message].join('\\n')))),b=>b.toString(16).padStart(2,'0')).join('');
  emit('ack',ack);
};`;
async function setupNotes(browser,options={}) {
  const h=await setup(browser,{...options,notesScript:script});
  const device={rows:new Map(),failLoad:false,failSave:false,failDelete:false,hold:null,calls:[]};
  await h.context.exposeBinding('__notesDeviceCall',async (_,{deviceId,operation,body})=>{
    device.calls.push({deviceId,operation,body});if(device.hold)await device.hold;
    const delay=Number(process.env.MAYAP_NOTES_QA_DELAY_MS || 0);
    if(delay>0)await new Promise(resolve=>setTimeout(resolve,delay));
    const rows=device.rows.get(deviceId)||[];
    if(operation==='notes.read')return device.failLoad?{ok:false,code:'NOTES_EEPROM_ERROR',message:'Không thể tải ghi chú. Thử lại.'}:{ok:true,code:'NOTES_DONE',rows};
    const save=operation==='notes.save';
    if(save?device.failSave:device.failDelete)return {ok:false,code:'NOTES_EEPROM_ERROR',message:save?'Không thể lưu ghi chú. Thử lại.':'Không thể xóa ghi chú. Thử lại.'};
    const old=rows.find(n=>n.id===body.note.id);
    if(old?old.version!==body.note.version:Boolean(body.note.version)||!save)return {ok:false,code:'NOTES_CONFLICT',message:'Ghi chú đã thay đổi; tải lại danh sách.'};
    if(save&&!old&&rows.length>=16)return {ok:false,code:'NOTES_FULL',message:'Đã đủ 16 ghi chú.'};
    const version=(old?.version||0)+1,next=rows.filter(n=>n.id!==body.note.id);
    if(save)next.push({...body.note,version});device.rows.set(deviceId,next);
    return {ok:true,code:'NOTES_STORED',version};
  });
  return {...h,device};
}
module.exports={setupNotes};
