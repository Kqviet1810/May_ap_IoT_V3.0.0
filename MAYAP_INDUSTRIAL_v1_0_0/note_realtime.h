#pragma once
// Included inside MayapRealtimeInternal after V2 ACK helpers.
struct NotePending {
  bool used=false,dataSent=false;char id[40]{},operation[40]{};uint8_t key[32]{};
  uint32_t receivedAt=0;
};
static NotePending notePending;
static MayapNoteJournal::Result noteResult;
static MayapNoteJournal::Document noteDocument;
inline void handleNoteRequest(const JsonDocument &doc){
  using namespace MayapNoteJournal;
  const char *id=doc["requestId"]|"",*action=doc["action"]|"";
  if(!activeAckKeyValid||!realtimeCommandChannelTrusted()){publishAck(id,"unauthorized","NOTE_JOURNAL_AUTH");return;}
  const time_t epoch=time(nullptr);
  if(!doc["expiresAt"].is<uint32_t>()||epoch<1700000000||doc["expiresAt"].as<uint32_t>()<static_cast<uint32_t>(epoch)||
     doc["expiresAt"].as<uint32_t>()>static_cast<uint32_t>(epoch)+30){publishAck(id,"rejected","NOTE_JOURNAL_EXPIRED");return;}
  if(notePending.used){publishAck(id,"busy","NOTE_JOURNAL_BUSY");return;}
  static Request request;request=Request{};
  if(!strcmp(action,"notes.list"))request.operation=Operation::List;
  else if(!strcmp(action,"notes.save"))request.operation=Operation::Save;
  else if(!strcmp(action,"notes.delete"))request.operation=Operation::Remove;
  else if(!strcmp(action,"notes.reminders.read"))request.operation=Operation::ReadReminders;
  else if(!strcmp(action,"notes.reminders.save"))request.operation=Operation::SaveReminders;
  else {publishAck(id,"invalid","NOTE_JOURNAL_INVALID");return;}
  Code mountCode;const auto mount=MayapNoteMailbox::status(mountCode);
  if(mount==MayapNoteMailbox::MountState::Mounting){publishAck(id,"busy","NOTE_JOURNAL_MOUNTING");return;}
  const bool mutation=request.operation==Operation::Save||request.operation==Operation::Remove||request.operation==Operation::SaveReminders;
  if(mutation&&mount!=MayapNoteMailbox::MountState::Ready){publishAck(id,"rejected",codeText(mountCode));return;}
  request.snapshot=!doc["generation"].isNull();
  if(request.snapshot&&!doc["generation"].is<uint32_t>()){publishAck(id,"invalid","NOTE_JOURNAL_INVALID");return;}
  request.generation=doc["generation"]|0UL;
  if(request.operation==Operation::ReadReminders||request.operation==Operation::SaveReminders){
    snprintf(request.note.id,sizeof(request.note.id),"%s",ReminderId);request.note.createdAt=1;request.note.type=2;
    if(request.operation==Operation::SaveReminders){
      if(!request.snapshot||!doc["version"].is<uint32_t>()||!doc["reminders"].is<JsonArrayConst>()||doc["reminders"].size()>10){publishAck(id,"invalid","NOTE_JOURNAL_INVALID");return;}
      request.note.version=doc["version"].as<uint32_t>();ReminderPayload payload;uint8_t slot=0;
      for(JsonObjectConst item:doc["reminders"].as<JsonArrayConst>()){
        const char *label=item["label"]|"";
        if(!item["day"].is<uint8_t>()||!item["day"].as<uint8_t>()||item["day"].as<uint8_t>()>200||strlen(label)>=80){publishAck(id,"invalid","NOTE_JOURNAL_INVALID");return;}
        payload.items[slot].day=item["day"].as<uint8_t>();memcpy(payload.items[slot].label,label,strlen(label)+1);++slot;
      }
      memcpy(request.note.content,&payload,sizeof(payload));
    }
  }else if(request.operation==Operation::List){
    if(!doc["cursor"].is<uint8_t>()){publishAck(id,"invalid","NOTE_JOURNAL_INVALID");return;}
    request.cursor=doc["cursor"].as<uint8_t>();
  }else{
    if(!request.snapshot||!doc["note"].is<JsonObjectConst>()){publishAck(id,"invalid","NOTE_JOURNAL_INVALID");return;}
    JsonObjectConst note=doc["note"].as<JsonObjectConst>();
    const char *noteId=note["id"]|"",*title=note["title"]|"",*content=note["content"]|"";
    if(strlen(noteId)>=sizeof(request.note.id)||strlen(title)>=sizeof(request.note.title)||strlen(content)>=sizeof(request.note.content)||
       !note["version"].is<uint32_t>()){publishAck(id,"invalid","NOTE_JOURNAL_INVALID");return;}
    memcpy(request.note.id,noteId,strlen(noteId)+1);request.note.version=note["version"].as<uint32_t>();
    if(request.operation==Operation::Save){
      if(!note["createdAt"].is<uint64_t>()){publishAck(id,"invalid","NOTE_JOURNAL_INVALID");return;}
      request.note.createdAt=note["createdAt"].as<uint64_t>();
      const char *type=note["type"]|"";request.note.type=!strcmp(type,"batch")?1:!strcmp(type,"machine")?2:0;
      memcpy(request.note.title,title,strlen(title)+1);memcpy(request.note.content,content,strlen(content)+1);
    }
  }
  portENTER_CRITICAL(&webMux);request.allowBatch=knownRuntimeValid&&knownRuntime.batchRunning;portEXIT_CRITICAL(&webMux);
  if(!MayapNoteMailbox::submit(request)){publishAck(id,"busy","NOTE_JOURNAL_BUSY");return;}
  notePending=NotePending{};notePending.used=true;notePending.receivedAt=millis();
  snprintf(notePending.id,sizeof(notePending.id),"%s",id);snprintf(notePending.operation,sizeof(notePending.operation),"%s",action);
  memcpy(notePending.key,activeAckKey,sizeof(notePending.key));
  mayapSerialPrintf(false,"[NOTE-JOURNAL] request=%s op=%s stage=RECEIVED generation=%lu\n",id,action,static_cast<unsigned long>(request.generation));
  publishAck(id,"accepted","NOTE_JOURNAL_ACCEPTED");
}
inline void serviceNoteResult(){
  if(!notePending.used||!MayapNoteMailbox::peek(noteResult,noteDocument))return;
  noteAckRevision=noteResult.generation;
  const bool reminder=!strncmp(notePending.operation,"notes.reminders.",16);
  if(reminder&&noteResult.code==MayapNoteJournal::Code::Ok&&MayapNoteMailbox::appliedRevision()!=noteResult.generation)return;
  if(noteResult.code==MayapNoteJournal::Code::Ok&&!notePending.dataSent){
    JsonDocument data;data["generation"]=noteResult.generation;data["next"]=noteResult.next;data["done"]=noteResult.done;
    if(reminder){data["version"]=noteResult.hasNote?noteDocument.version:0;
      auto list=data["reminders"].to<JsonArray>();MayapNoteJournal::ReminderPayload payload{};
      if(noteResult.hasNote)memcpy(&payload,noteDocument.content,sizeof(payload));
      for(const auto &item:payload.items)if(item.day){auto entry=list.add<JsonObject>();entry["day"]=item.day;entry["label"]=String(item.label); /* JSON owns bytes beyond payload scope. */}
    }else if(noteResult.hasNote){auto note=data["note"].to<JsonObject>();note["id"]=noteDocument.id;note["version"]=static_cast<uint32_t>(noteDocument.version);
      note["createdAt"]=static_cast<uint64_t>(noteDocument.createdAt);note["type"]=noteDocument.type==1?"batch":"machine";
      note["title"]=noteDocument.title;note["content"]=noteDocument.content;}
    String body;serializeJson(data,body);
    String signedText=String("mayap-note-journal:v2\n")+deviceId+"\n"+String(bootId)+"\n"+notePending.id+"\n"+notePending.operation+"\n"+body;
    uint8_t digest[32];const auto *info=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if(!info||mbedtls_md_hmac(info,notePending.key,32,reinterpret_cast<const uint8_t *>(signedText.c_str()),signedText.length(),digest))return;
    char hex[65];for(uint8_t i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",digest[i]);
    JsonDocument frame;frame["v"]=2;frame["bootId"]=bootId;frame["requestId"]=notePending.id;
    frame["operation"]=notePending.operation;frame["body"]=body;frame["sig"]=hex;
    if(!publishJson("notes/reported",frame,false))return;
    notePending.dataSent=true;
  }
  const bool ok=noteResult.code==MayapNoteJournal::Code::Ok;
  if(!publishAck(notePending.id,ok?"applied":"rejected",MayapNoteJournal::codeText(noteResult.code),notePending.operation,
      notePending.receivedAt,millis(),notePending.key))return;
  mayapSerialPrintf(false,"[NOTE-JOURNAL] request=%s op=%s stage=COMPLETED generation=%lu code=%s addr=0x%04X\n",
    notePending.id,notePending.operation,static_cast<unsigned long>(noteResult.generation),MayapNoteJournal::codeText(noteResult.code),noteResult.address);
  MayapNoteMailbox::consume();notePending=NotePending{};
}
