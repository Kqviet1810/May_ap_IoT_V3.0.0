// Included inside MayapRealtimeInternal after the existing signed ACK helpers.
// Network owner only: metadata is reserved before storage work becomes visible.
struct NotesPending {
  bool used=false, waiting=false, ready=false, uncertainSent=false;
  uint32_t token=0, receivedAt=0;
  char requestId[WEB_REQUEST_ID_CAPACITY]{}, operation[20]{};
  uint8_t key[32]{};
  bool signedAck=false;
  MayapNotes::Request job{};
  MayapNotes::Response response{};
};
static NotesPending notesPending;
static uint32_t notesToken=0;
inline void handleNotesMessage(const JsonDocument &doc,bool read) {
  using namespace MayapNotes;
  const char *id=doc["requestId"]|"";
  if(!id[0])return;
  if(notesPending.used){publishAck(id,"busy","NOTES_BUSY");return;}
  Request job{};job.token=++notesToken;if(!job.token)job.token=++notesToken;
  if(read)job.operation=Operation::List;
  else {
    const char *action=doc["action"]|"";
    if(!strcmp(action,"save"))job.operation=Operation::Upsert;
    else if(!strcmp(action,"delete"))job.operation=Operation::Remove;
    else {publishAck(id,"invalid","NOTES_INVALID");return;}
    JsonVariantConst value=doc["note"];
    const char *noteId=value["id"]|"";
    if(!validId(noteId) || !value["version"].is<uint32_t>()){publishAck(id,"invalid","NOTES_INVALID");return;}
    memcpy(job.note.id,noteId,37);job.note.version=value["version"].as<uint32_t>();
    if(job.operation==Operation::Upsert){
      const char *title=value["title"]|"",*content=value["content"]|"",*type=value["type"]|"";
      if(strlen(title)>=sizeof(job.note.title)||strlen(content)>=sizeof(job.note.content)||!value["createdAt"].is<uint64_t>() ||
          (strcmp(type,"batch")&&strcmp(type,"machine"))){publishAck(id,"invalid","NOTES_INVALID");return;}
      memcpy(job.note.title,title,strlen(title)+1);memcpy(job.note.content,content,strlen(content)+1);
      job.note.createdAt=value["createdAt"].as<uint64_t>();job.note.type=!strcmp(type,"batch")?1:2;
      if(!validNote(job.note)){publishAck(id,"invalid","NOTES_INVALID");return;}
      portENTER_CRITICAL(&webMux);job.allowBatch=knownRuntimeValid&&knownRuntime.batchRunning;portEXIT_CRITICAL(&webMux);
    }
  }
  notesPending=NotesPending{};notesPending.used=true;notesPending.waiting=true;notesPending.token=job.token;notesPending.receivedAt=millis();notesPending.job=job;
  snprintf(notesPending.requestId,sizeof(notesPending.requestId),"%s",id);
  snprintf(notesPending.operation,sizeof(notesPending.operation),"%s",read?"notes.read":job.operation==Operation::Upsert?"notes.save":"notes.delete");
  notesPending.signedAck=activeAckKeyValid;if(activeAckKeyValid)memcpy(notesPending.key,activeAckKey,32);
  if(!mayapNotesSubmit(job)){notesPending.used=false;publishAck(id,"busy","NOTES_BUSY");return;}
  publishAck(id,"accepted","NOTES_RECEIVED");
}
inline void serviceNotes() {
  using namespace MayapNotes;
  if(!notesPending.used)return;
  if(notesPending.waiting && mayapNotesReceive(notesPending.response)) {
    if(notesPending.response.token==notesPending.token){notesPending.waiting=false;notesPending.ready=true;}
  }
  if(!notesPending.ready || !socketTransport.connected())return;
  auto &result=notesPending.response;
  if(result.ambiguous) {
    if(!notesPending.uncertainSent && !publishAck(notesPending.requestId,"expired","NOTES_UNCERTAIN",notesPending.operation,notesPending.receivedAt,millis(),notesPending.signedAck?notesPending.key:nullptr))return;
    if(!notesPending.uncertainSent)mayapSerialPrintf(false,
      "[NOTES] uncertain token=%lu op=%s stage=%s addr=0x%04X mismatch=%d blank=%u valid=%u code=%s write_reason=%u failed_addr=0x%04X requested=%u written=%u wire_error=%u\n",
      static_cast<unsigned long>(notesPending.token),notesPending.operation,stageText(result.ioStage),
      static_cast<unsigned>(result.ioAddress),static_cast<int>(result.mismatch),result.blank?1U:0U,result.validReadback?1U:0U,codeText(result.code),result.writeReason,static_cast<unsigned>(result.failedAddress),result.requested,result.written,result.wireError);
    notesPending.uncertainSent=true;notesPending.ready=false;notesPending.waiting=true;return;
  }
  if(result.code==Code::Ok && notesPending.job.operation==Operation::List) {
    JsonDocument doc;doc["v"]=1;doc["bootId"]=bootId;doc["requestId"]=notesPending.requestId;
    doc["cursor"]=notesPending.job.cursor;doc["nextCursor"]=result.nextCursor;doc["done"]=result.done;doc["capacity"]=CAPACITY;
    JsonArray items=doc["notes"].to<JsonArray>();
    if(result.hasNote){JsonObject n=items.add<JsonObject>();n["id"]=result.note.id;n["version"]=result.note.version;n["createdAt"]=result.note.createdAt;n["type"]=result.note.type==1?"batch":"machine";n["title"]=result.note.title;n["content"]=result.note.content;}
    // Cursor changes only after the frame enters the bounded transport queue.
    if(!publishJson("notes/reported",doc,false))return;
    if(!result.done){notesPending.job.cursor=result.nextCursor;notesPending.ready=false;return;}
  }
  const bool ok=result.code==Code::Ok;
  const char *terminalCode=ok?(notesPending.job.operation==Operation::List?"NOTES_DONE":"NOTES_STORED"):
    (result.retryExhausted?"NOTES_RETRY_EXHAUSTED":codeText(result.code));
  notesAckRevision=result.note.version;
  if(!publishAck(notesPending.requestId,ok?"applied":"rejected",terminalCode,notesPending.operation,notesPending.receivedAt,millis(),notesPending.signedAck?notesPending.key:nullptr))return;
  mayapSerialPrintf(false,"[NOTES] terminal token=%lu op=%s ok=%u version=%lu code=%s retries=%u exhausted=%u\n",
    static_cast<unsigned long>(notesPending.token),notesPending.operation,ok?1U:0U,
    static_cast<unsigned long>(notesAckRevision),terminalCode,result.reconcileAttempts,result.retryExhausted?1U:0U);
  notesPending.used=false;
}
inline void serviceNotesAdmission() {
  if(notesPending.used&&!notesPending.waiting&&!notesPending.ready) {
    if(mayapNotesSubmit(notesPending.job))notesPending.waiting=true;
  }
}
