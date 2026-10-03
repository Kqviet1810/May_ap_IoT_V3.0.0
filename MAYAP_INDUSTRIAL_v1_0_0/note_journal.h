#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Append-only page journal. Exactly one bounded 32-byte I/O per step.
// No heap, GPIO, network, formatting or writes outside the notes reservation.
namespace MayapNoteJournal {
constexpr uint16_t Base=0x3000, SlotBytes=2048, Chunk=32;
constexpr uint8_t Slots=24, Capacity=16;
constexpr uint32_t End=uint32_t(Base)+uint32_t(Slots)*SlotBytes;
constexpr uint32_t Magic=0x4E4A5632, SealMagic=0x4E4A434D;
inline uint32_t crc(const void *data,size_t size){
  uint32_t c=~0U;const uint8_t *p=static_cast<const uint8_t *>(data);
  while(size--){c^=*p++;for(uint8_t b=0;b<8;++b)c=(c>>1)^((c&1)?0xEDB88320U:0);}
  return ~c;
}
inline bool idValid(const char *id){
  if(strnlen(id,37)!=36)return false;
  for(uint8_t i=0;i<36;++i){const char c=id[i];
    if(i==8||i==13||i==18||i==23){if(c!='-')return false;}
    else if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
  }return true;
}
inline bool textValid(const char *p,size_t capacity,uint16_t maxUnits,bool required){
  const size_t n=strnlen(p,capacity);if(n==capacity)return false;
  uint16_t units=0;bool visible=false;
  for(size_t i=0;i<n;){uint32_t c=uint8_t(p[i++]),minimum=0;uint8_t more=0;
    if(c>=0xC2&&c<=0xDF){c&=31;more=1;minimum=0x80;}
    else if(c>=0xE0&&c<=0xEF){c&=15;more=2;minimum=0x800;}
    else if(c>=0xF0&&c<=0xF4){c&=7;more=3;minimum=0x10000;}
    else if(c>=0x80)return false;
    if(i+more>n)return false;
    while(more--){const uint8_t b=uint8_t(p[i++]);if((b&0xC0)!=0x80)return false;c=(c<<6)|(b&63);}
    if(c<minimum||c>0x10FFFF||(c>=0xD800&&c<=0xDFFF)||(c<32&&c!=9&&c!=10&&c!=13)||c==127)return false;
    units+=c>0xFFFF?2:1;if(units>maxUnits)return false;
    visible|=!(c==9||c==10||c==13||c==32||c==0x85||c==0xA0||c==0x1680||
      (c>=0x2000&&c<=0x200A)||c==0x2028||c==0x2029||c==0x202F||c==0x205F||c==0x3000||c==0xFEFF);
  }return !required||visible;
}
#pragma pack(push,1)
struct Document {
  char id[37]{};uint32_t version=0;uint64_t createdAt=0;uint8_t type=2;
  char title[241]{},content[1201]{};
};
struct Reminder {uint8_t day=0;char label[80]{};};
struct ReminderPayload {Reminder items[10]{};};
struct Record {
  uint32_t magic=Magic;uint16_t schema=2,size=sizeof(Record);
  uint32_t sequence=0;uint8_t live=1,kind=1,reserved[2]{};Document note{};uint32_t checksum=0;
};
struct Seal {uint32_t magic=SealMagic,sequence=0,checksum=0,check=0;uint8_t reserved[16]{};};
#pragma pack(pop)
static_assert(sizeof(Seal)==Chunk && sizeof(Record)<SlotBytes-128,"Journal page layout");
inline bool validDocument(const Document &n){return idValid(n.id)&&n.createdAt>0&&n.createdAt<=8640000000000000ULL&&
  (n.type==1||n.type==2)&&textValid(n.title,sizeof(n.title),60,false)&&textValid(n.content,sizeof(n.content),300,true);}
constexpr const char *ReminderId="00000000-0000-4000-8000-000000000000";
inline bool validReminderPayload(const Document &n){
  if(strcmp(n.id,ReminderId)||n.type!=2||n.createdAt!=1)return false;
  ReminderPayload p;memcpy(&p,n.content,sizeof(p));
  for(const auto &item:p.items){if(item.day>200||(!item.day&&item.label[0])||
    (item.day&&!textValid(item.label,sizeof(item.label),79,true)))return false;}
  return true;
}
inline bool validRecord(const Record &r){return r.magic==Magic&&r.schema==2&&r.size==sizeof(r)&&r.sequence&&r.live<=1&&
  r.reserved[0]==0&&r.reserved[1]==0&&r.note.version==r.sequence&&
  ((r.kind==1&&strcmp(r.note.id,ReminderId)&&validDocument(r.note))||(r.kind==2&&r.live&&validReminderPayload(r.note)))&&r.checksum==crc(&r,offsetof(Record,checksum));}
inline bool validSeal(const Seal &s){for(uint8_t b:s.reserved)if(b)return false;return s.magic==SealMagic&&s.sequence&&s.check==crc(&s,offsetof(Seal,check));}
enum class Operation:uint8_t {List,Save,Remove,ReadReminders,SaveReminders};
enum class Code:uint8_t {Ok,Io,Corrupt,Full,Conflict,Invalid};
inline const char *codeText(Code c){switch(c){case Code::Ok:return "NOTE_JOURNAL_OK";case Code::Io:return "NOTE_JOURNAL_IO";
  case Code::Corrupt:return "NOTE_JOURNAL_CORRUPT";case Code::Full:return "NOTE_JOURNAL_FULL";
  case Code::Conflict:return "NOTE_JOURNAL_CONFLICT";default:return "NOTE_JOURNAL_INVALID";}}
struct Request {Operation operation=Operation::List;uint32_t generation=0;uint8_t cursor=0;bool snapshot=false,allowBatch=false;Document note{};};
struct Result {Code code=Code::Ok;uint32_t generation=0;uint8_t next=0;bool done=true,hasNote=false;uint16_t address=0;};

template<class Io> class Journal {
  struct Index {char id[37]{};uint32_t sequence=0;uint8_t slot=Slots,kind=1;bool live=false;};
  enum class Phase:uint8_t {Idle,MountSeal,MountBody,Start,Load,Invalidate,VerifyInvalid,Write,Verify,Commit,VerifySeal,FinalRead,Done};
  Io &io_;Index index_[Slots]{};Record record_{};Seal seal_{};Request request_{};Result result_{};
  Phase phase_=Phase::Idle;bool mounted_=false;uint8_t scan_=0,target_=0,entry_=0,latest_=Slots,retries_=0;
  uint16_t offset_=0;uint32_t generation_=0;
  uint16_t address(uint8_t slot,uint16_t offset)const{return Base+slot*SlotBytes+offset;}
  void finish(Code code){result_.code=code;result_.generation=generation_;phase_=Phase::Done;if(code==Code::Io||code==Code::Corrupt)mounted_=false;}
  bool read(uint16_t addr,void *data,size_t n){result_.address=addr;if(io_.readBytes(addr,data,n))return true;finish(Code::Io);return false;}
  void acceptRecord(){
    uint8_t i=Slots;for(uint8_t n=0;n<Slots;++n)if(!strcmp(index_[n].id,record_.note.id)){i=n;break;}
    if(i==Slots){for(uint8_t n=0;n<Slots;++n)if(!index_[n].sequence){i=n;break;}}
    if(i==Slots){finish(Code::Corrupt);return;}
    if(record_.sequence>index_[i].sequence){auto &e=index_[i];memcpy(e.id,record_.note.id,sizeof(e.id));e.sequence=record_.sequence;e.slot=scan_;e.live=record_.live;e.kind=record_.kind;}
  }
  void nextScan(){if(++scan_==Slots){uint8_t live=0;for(const auto &e:index_)live+=e.sequence&&e.live&&e.kind==1;
    if(live>Capacity){finish(Code::Corrupt);return;}mounted_=true;phase_=Phase::Start;}else phase_=Phase::MountSeal;}
  bool matching()const{return validRecord(record_)&&record_.sequence==seal_.sequence&&record_.checksum==seal_.checksum;}
  void start(){
    if(request_.cursor>Slots||(!idValid(request_.note.id)&&request_.operation!=Operation::List)){finish(Code::Invalid);return;}
    if(request_.operation==Operation::List){
      if(request_.snapshot&&request_.generation!=generation_){finish(Code::Conflict);return;}
      entry_=request_.cursor;while(entry_<Slots&&(!index_[entry_].live||index_[entry_].kind!=1))++entry_;
      result_.next=entry_<Slots?entry_+1:Slots;result_.done=result_.next==Slots;
      if(entry_==Slots){finish(Code::Ok);return;}
      target_=index_[entry_].slot;offset_=0;phase_=Phase::Load;return;
    }
    if((request_.operation==Operation::Save||request_.operation==Operation::Remove)&&!strcmp(request_.note.id,ReminderId)){finish(Code::Invalid);return;}
    if(request_.operation==Operation::SaveReminders&&!validReminderPayload(request_.note)){finish(Code::Invalid);return;}
    if(request_.operation==Operation::Save&&!validDocument(request_.note)){finish(Code::Invalid);return;}
    entry_=Slots;for(uint8_t i=0;i<Slots;++i)if(index_[i].sequence&&!strcmp(index_[i].id,request_.note.id)){entry_=i;break;}
    if(entry_<Slots){target_=index_[entry_].slot;offset_=0;phase_=Phase::Load;return;}
    if(request_.operation==Operation::ReadReminders){finish(Code::Ok);return;}
    if(request_.generation!=generation_||request_.note.version){finish(Code::Conflict);return;}
    if(request_.operation==Operation::Remove){finish(Code::Ok);return;}
    prepare();
  }
  void loaded(){
    if(!validRecord(record_)||record_.sequence!=index_[entry_].sequence||strcmp(record_.note.id,index_[entry_].id)){finish(Code::Corrupt);return;}
    if(request_.operation==Operation::List||request_.operation==Operation::ReadReminders){result_.hasNote=true;finish(Code::Ok);return;}
    if(request_.operation==Operation::Remove&&!record_.live){finish(Code::Ok);return;}
    Document candidate=request_.note;candidate.version=record_.note.version;candidate.createdAt=record_.note.createdAt;
    if((request_.operation==Operation::Save||request_.operation==Operation::SaveReminders)&&record_.live&&!memcmp(&candidate,&record_.note,sizeof(candidate))){result_.hasNote=true;finish(Code::Ok);return;}
    if(request_.generation!=generation_||!record_.live||request_.note.version!=record_.sequence){finish(Code::Conflict);return;}
    if(request_.operation==Operation::Remove)request_.note=record_.note;
    else request_.note.createdAt=record_.note.createdAt;
    prepare();
  }
  void prepare(){
    if(generation_==UINT32_MAX){finish(Code::Full);return;}
    if(request_.note.type==1&&!request_.allowBatch&&(entry_==Slots||record_.note.type!=1)){finish(Code::Invalid);return;}
    if(entry_==Slots){uint8_t live=0;for(const auto &e:index_)live+=e.sequence&&e.live&&e.kind==1;
      if(live>=Capacity&&request_.operation!=Operation::SaveReminders){finish(Code::Full);return;}
      for(uint8_t i=0;i<Slots;++i)if(!index_[i].sequence){entry_=i;break;}
      // Keep latest tombstones indexed while older physical copies can exist.
      // A full index accounts for every physical slot; then a retired tombstone
      // has no older copy and can supply both an index entry and its slot.
      if(entry_==Slots)for(uint8_t i=0;i<Slots;++i)if(!index_[i].live&&index_[i].slot!=latest_){entry_=i;break;}
    }
    if(entry_==Slots){finish(Code::Full);return;}
    target_=Slots;
    for(uint8_t k=0;k<Slots;++k){const uint8_t s=(latest_==Slots?k:(latest_+1+k)%Slots);bool used=s==latest_;for(const auto &e:index_)used|=e.sequence&&e.slot==s;
      if(!used){target_=s;break;}}
    if(target_==Slots){
      // Every slot holds a distinct latest ID: therefore a retired tombstone
      // has no older live record left to resurrect. Preserve the latest seq.
      for(uint8_t i=0;i<Slots;++i)if(index_[i].sequence&&!index_[i].live&&index_[i].slot!=latest_){target_=index_[i].slot;break;}
      if(target_==Slots){finish(Code::Corrupt);return;}
    }
    record_=Record{};record_.sequence=generation_+1;record_.live=request_.operation!=Operation::Remove;record_.kind=request_.operation==Operation::SaveReminders?2:1;
    record_.note=request_.note;record_.note.version=record_.sequence;record_.checksum=crc(&record_,offsetof(Record,checksum));
    seal_=Seal{};seal_.sequence=record_.sequence;seal_.checksum=record_.checksum;seal_.check=crc(&seal_,offsetof(Seal,check));
    offset_=0;retries_=0;phase_=Phase::Invalidate;
  }
 public:
  explicit Journal(Io &io):io_(io){}
  bool begin(const Request &request){
    if(phase_!=Phase::Idle&&phase_!=Phase::Done)return false;
    request_=request;result_=Result{};
    if(mounted_)phase_=Phase::Start;
    else {for(auto &e:index_)e=Index{};generation_=0;latest_=Slots;scan_=0;phase_=Phase::MountSeal;}
    return true;
  }
  bool ready()const{return phase_==Phase::Done;}
  const Result &result()const{return result_;}
  const Document &document()const{return record_.note;}
  void step(){
    uint8_t bytes[Chunk]{};const uint16_t sealOffset=SlotBytes-Chunk;
    const size_t n=offset_<sizeof(Record)?(sizeof(Record)-offset_<Chunk?sizeof(Record)-offset_:Chunk):0;
    switch(phase_){
      case Phase::MountSeal:
        if(!read(address(scan_,sealOffset),&seal_,sizeof(seal_)))return;
        if(!validSeal(seal_)){nextScan();return;}
        record_=Record{};offset_=0;phase_=Phase::MountBody;return;
      case Phase::MountBody:
        if(!read(address(scan_,offset_),reinterpret_cast<uint8_t *>(&record_)+offset_,n))return;
        offset_+=n;if(offset_<sizeof(Record))return;
        if(!matching()){finish(Code::Corrupt);return;}
        if(record_.sequence>generation_){generation_=record_.sequence;latest_=scan_;}
        acceptRecord();if(phase_!=Phase::Done)nextScan();return;
      case Phase::Start:start();return;
      case Phase::Load:
        if(!read(address(target_,offset_),reinterpret_cast<uint8_t *>(&record_)+offset_,n))return;
        offset_+=n;if(offset_==sizeof(Record))loaded();return;
      case Phase::Invalidate:
        memset(bytes,0xFF,sizeof(bytes));result_.address=address(target_,sealOffset);
        (void)io_.writeBytes(result_.address,bytes,sizeof(bytes));phase_=Phase::VerifyInvalid;return;
      case Phase::VerifyInvalid:
        if(!read(address(target_,sealOffset),bytes,sizeof(bytes)))return;
        for(uint8_t b:bytes)if(b!=0xFF){if(++retries_<3){phase_=Phase::Invalidate;return;}finish(Code::Io);return;}
        retries_=0;phase_=Phase::Write;return;
      case Phase::Write:
        result_.address=address(target_,offset_);(void)io_.writeBytes(result_.address,reinterpret_cast<const uint8_t *>(&record_)+offset_,n);
        phase_=Phase::Verify;return;
      case Phase::Verify:
        if(!read(address(target_,offset_),bytes,n))return;
        if(memcmp(bytes,reinterpret_cast<const uint8_t *>(&record_)+offset_,n)){if(++retries_<3){phase_=Phase::Write;return;}finish(Code::Io);return;}
        retries_=0;offset_+=n;phase_=offset_==sizeof(Record)?Phase::Commit:Phase::Write;return;
      case Phase::Commit:
        result_.address=address(target_,sealOffset);(void)io_.writeBytes(result_.address,&seal_,sizeof(seal_));phase_=Phase::VerifySeal;return;
      case Phase::VerifySeal:
        if(!read(address(target_,sealOffset),bytes,sizeof(bytes)))return;
        if(memcmp(bytes,&seal_,sizeof(seal_))){if(++retries_<3){phase_=Phase::Commit;return;}finish(Code::Io);return;}
        offset_=0;phase_=Phase::FinalRead;return;
      case Phase::FinalRead:{
        if(!read(address(target_,offset_),bytes,n))return;
        if(memcmp(bytes,reinterpret_cast<const uint8_t *>(&record_)+offset_,n)){finish(Code::Corrupt);return;}
        offset_+=n;if(offset_<sizeof(Record))return;
        for(uint8_t i=0;i<Slots;++i)if(i!=entry_&&index_[i].sequence&&index_[i].slot==target_)index_[i]=Index{};
        generation_=record_.sequence;latest_=target_;auto &e=index_[entry_];memcpy(e.id,record_.note.id,sizeof(e.id));
        e.sequence=generation_;e.slot=target_;e.live=record_.live;e.kind=record_.kind;result_.hasNote=record_.live;finish(Code::Ok);return;}
      default:return;
    }
  }
};
}
