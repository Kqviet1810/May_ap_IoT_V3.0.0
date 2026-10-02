#include <ArduinoJson.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "notes_store.h"
constexpr size_t WEB_REQUEST_ID_CAPACITY=40;
static uint32_t clockMs=100,bootId=123,notesAckRevision=0;
uint32_t millis(){return clockMs;}
static int webMux;static bool knownRuntimeValid=true;static struct{bool batchRunning=true;}knownRuntime;
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static bool activeAckKeyValid=true;static uint8_t activeAckKey[32]={7};
static struct{bool connected(){return true;}}socketTransport;
static bool failSend=false,available=true,hasResult=false;
static MayapNotes::Request submitted;static MayapNotes::Response response;
static std::vector<std::string>acks;static std::vector<unsigned>cursors;
bool publishAck(const char*,const char *result,const char*,const char * = "",uint32_t=0,uint32_t=0,const uint8_t * = nullptr);
bool publishJson(const char*,const JsonDocument &doc,bool){if(failSend)return false;cursors.push_back(doc["cursor"].as<unsigned>());return true;}
#include "notes_link.h"
bool publishAck(const char*,const char *result,const char*,const char*,uint32_t,uint32_t,const uint8_t*){if(failSend)return false;acks.push_back(result);return true;}
bool mayapNotesSubmit(const MayapNotes::Request &job){if(!available)return false;assert(notesPending.used&&notesPending.token==job.token&&notesPending.signedAck&&notesPending.requestId[0]);submitted=job;return true;}
bool mayapNotesReceive(MayapNotes::Response &out){if(!hasResult)return false;out=response;hasResult=false;return true;}
JsonDocument read(){JsonDocument d;d["requestId"]="notes-read";return d;}
JsonDocument write(){auto d=read();d["action"]="save";auto n=d["note"].to<JsonObject>();n["id"]="00000000-0000-4000-8000-000000000001";n["version"]=0;n["createdAt"]=1790000000000ULL;n["type"]="machine";n["title"]="";n["content"]="Bảo trì máy";return d;}
void finish(bool done,uint8_t next,MayapNotes::Code code=MayapNotes::Code::Ok,bool ambiguous=false){response=MayapNotes::Response{};response.token=submitted.token;response.done=done;response.nextCursor=next;response.code=code;response.ambiguous=ambiguous;hasResult=true;}
int main(){
 available=false;handleNotesMessage(read(),true);assert(!notesPending.used);available=true;acks.clear();
 handleNotesMessage(read(),true);assert(notesPending.used&&submitted.cursor==0&&acks.back()=="accepted");
 finish(false,1);failSend=true;serviceNotes();assert(notesPending.used&&notesPending.job.cursor==0&&notesPending.ready);
 failSend=false;serviceNotes();assert(notesPending.job.cursor==1&&acks.size()==1);serviceNotesAdmission();assert(submitted.cursor==1);
 finish(true,16);failSend=true;serviceNotes();assert(notesPending.used);failSend=false;serviceNotes();assert(!notesPending.used&&acks.back()=="applied");
 assert(cursors.size()==2&&cursors[0]==0&&cursors[1]==1);
 acks.clear();handleNotesMessage(write(),false);assert(submitted.note.version==0);
 finish(true,0,MayapNotes::Code::Eeprom,true);serviceNotes();assert(notesPending.used&&notesPending.uncertainSent&&acks.back()=="expired");
 finish(true,0,MayapNotes::Code::Eeprom,true);serviceNotes();assert(acks.size()==2); // One uncertain frame, bounded logging/traffic.
 finish(true,0);response.note=submitted.note;response.note.version=3;failSend=true;serviceNotes();assert(notesPending.used);
 failSend=false;serviceNotes();assert(!notesPending.used&&acks.back()=="applied"&&notesAckRevision==3);
 auto invalid=write();invalid["note"]["content"]="   ";handleNotesMessage(invalid,false);assert(!notesPending.used&&acks.back()=="invalid");
 puts("Actual notes link: admission metadata, failed-send cursor fence, read completion, uncertain readback and retained terminal ACK PASS");
}
