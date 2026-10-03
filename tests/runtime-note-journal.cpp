// Actual journal, storage/mailbox and realtime handlers; hardware/crypto HALs only.
#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <new>
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
static int depth=0;
#define portENTER_CRITICAL(x) (++depth,(void)(x))
#define portEXIT_CRITICAL(x) (--depth,(void)(x))
struct ReminderItem {uint8_t day=0;char label[80]{};};
struct ReminderSet {ReminderItem items[10];};
class String:public std::string {
 public:using std::string::string;String(const std::string&s):std::string(s){}
  explicit String(uint32_t n):std::string(std::to_string(n)){}
  size_t length()const{return size();}
  size_t write(uint8_t c){push_back(char(c));return 1;}
  size_t write(const uint8_t*p,size_t n){append(reinterpret_cast<const char*>(p),n);return n;}
};
static uint32_t clockMs=100;uint32_t millis(){return clockMs;}
static std::vector<uint8_t> bytes(65536,0xff);static int ioCalls=0,writes=0;static bool wp=false;
namespace Mayap {class ExternalEeprom24xx {public:
 struct WriteTrace {uint16_t address=0;uint8_t reason=0,requested=0,written=0,error=0;};
 WriteTrace lastWriteTrace()const{return {};}
 bool readBytes(uint16_t a,void*p,size_t n){assert(!depth&&n<=32);++ioCalls;memcpy(p,&bytes[a],n);return true;}
 bool writeBytes(uint16_t a,const void*p,size_t n){assert(!depth&&n<=32);++ioCalls;++writes;if(wp)return false;memcpy(&bytes[a],p,n);return true;}
};}
constexpr uint32_t EEPROM_ADDR_TEMP_HISTORY=0x1000,TEMP_HISTORY_STORAGE_BYTES=8064,EEPROM_CAPACITY_BYTES=65536;
void mayapSerialPrintf(bool,const char*,...){}
#include "../MAYAP_INDUSTRIAL_v1_0_0/note_storage.h"
namespace Runtime {
static bool activeAckKeyValid=true,trusted=true;static uint8_t activeAckKey[32]={7};
static int webMux;static struct{bool batchRunning=false;}knownRuntime;static bool knownRuntimeValid=true;
static char deviceId[]="MAP-1234567890AB";static uint32_t bootId=123,noteAckRevision=0;
bool realtimeCommandChannelTrusted(){return trusted;}
struct mbedtls_md_info_t{};constexpr int MBEDTLS_MD_SHA256=1;
const mbedtls_md_info_t*mbedtls_md_info_from_type(int){static mbedtls_md_info_t info;return &info;}
int mbedtls_md_hmac(const mbedtls_md_info_t*,const uint8_t*,size_t,const uint8_t*,size_t,uint8_t*digest){memset(digest,7,32);return 0;}
static std::vector<std::string> ackCodes,dataFrames;static bool failData=false,failAck=false;
bool publishAck(const char*,const char*,const char*code,const char* = "",uint32_t=0,uint32_t=0,const uint8_t* = nullptr){
 if(failAck)return false;
 ackCodes.push_back(code);return true;}
bool publishJson(const char*channel,const JsonDocument&doc,bool){assert(!strcmp(channel,"notes/reported"));if(failData)return false;
 std::string frame;serializeJson(doc,frame);assert(frame.size()+50<=2048);assert(doc["body"].as<std::string>().size()<1700);dataFrames.push_back(frame);return true;}
#include "../MAYAP_INDUSTRIAL_v1_0_0/note_realtime.h"
}
using namespace Runtime;
void step(){ioCalls=0;MayapNoteStorage::update();assert(ioCalls<=1);clockMs+=10;}
void finishStorage(){for(int i=0;i<10000&&!MayapNoteMailbox::ready;i++)step();assert(MayapNoteMailbox::ready);}
JsonDocument request(const char*action,uint32_t generation=0){JsonDocument d;d["requestId"]="jnl-1";d["action"]=action;d["expiresAt"]=time(nullptr)+30;d["generation"]=generation;return d;}
void apply(){ReminderSet r;uint32_t version;assert(MayapNoteMailbox::takeReminders(r,version));MayapNoteMailbox::applied(version);}
int main(){
 for(int i=0;i<1000&&!MayapNoteStorage::bootFinished;i++){step();}
 assert(MayapNoteStorage::bootFinished&&!MayapNoteMailbox::busy);apply();
 auto d=request("notes.reminders.save");d["version"]=0;auto list=d["reminders"].to<JsonArray>();auto entry=list.add<JsonObject>();entry["day"]=7;entry["label"]="Soi trứng: tiếng Việt ư, đ";
 handleNoteRequest(d);assert(notePending.used&&MayapNoteMailbox::busy);handleNoteRequest(d);assert(ackCodes.back()=="NOTE_JOURNAL_BUSY");finishStorage();
 serviceNoteResult();assert(dataFrames.empty()&&notePending.used); // No ACK until controller applied.
 apply();failData=true;serviceNoteResult();assert(dataFrames.empty()&&MayapNoteMailbox::ready);const int committedWrites=writes;
 failData=false;failAck=true;serviceNoteResult();assert(dataFrames.size()==1&&MayapNoteMailbox::ready);
 failAck=false;serviceNoteResult();assert(dataFrames.size()==1&&!MayapNoteMailbox::busy&&!notePending.used&&noteAckRevision==1);
 assert(ackCodes.back()=="NOTE_JOURNAL_OK"&&writes==committedWrites);
 // Repeating the same desired list is verified but causes no new EEPROM write.
 handleNoteRequest(d);finishStorage();apply();serviceNoteResult();assert(writes==committedWrites);
 auto note=request("notes.save",1);auto n=note["note"].to<JsonObject>();n["id"]="00000000-0000-4000-8000-000000000001";n["version"]=0;n["createdAt"]=1750000000000ULL;n["type"]="machine";
 n["title"]=std::string(60,'"');n["content"]="x"+std::string(299,'\\'); // Worst JSON re-escaping, within UI limits.
 handleNoteRequest(note);finishStorage();serviceNoteResult();assert(noteAckRevision==2&&ackCodes.back()=="NOTE_JOURNAL_OK");
 wp=true;note["generation"]=2;n["version"]=2;n["content"]="A write that must not be acknowledged as saved";handleNoteRequest(note);finishStorage();
 const auto frames=dataFrames.size();serviceNoteResult();assert(dataFrames.size()==frames&&ackCodes.back()=="NOTE_JOURNAL_IO");wp=false;
 // Reboot restores the verified reminder without any network service.
 new (&MayapNoteStorage::journal) decltype(MayapNoteStorage::journal)(MayapNoteStorage::io);
 MayapNoteStorage::bootStarted=MayapNoteStorage::bootFinished=false;
 for(int i=0;i<10000&&!MayapNoteStorage::bootFinished;i++){step();}
 ReminderSet restored;uint32_t revision;
 assert(MayapNoteMailbox::takeReminders(restored,revision));assert(restored.items[0].day==7&&!strcmp(restored.items[0].label,"Soi trứng: tiếng Việt ư, đ"));
 assert(revision==2&&!MayapNoteMailbox::busy);
 trusted=false;auto read=request("notes.reminders.read");handleNoteRequest(read);assert(!MayapNoteMailbox::busy&&ackCodes.back()=="NOTE_JOURNAL_AUTH");trusted=true;
 read["expiresAt"]=time(nullptr)-1;handleNoteRequest(read);assert(!MayapNoteMailbox::busy&&ackCodes.back()=="NOTE_JOURNAL_EXPIRED");
 puts("Actual shared journal pipeline: offline boot restore, verified runtime application before ACK, busy, failed DATA/ACK retention, no-op, maximum escaping, write protection and auth/expiry PASS");
}
