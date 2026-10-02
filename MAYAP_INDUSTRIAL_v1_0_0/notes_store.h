#pragma once
// Portable storage core. No network, controller, heap allocation or scheduling.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace MayapNotes {
constexpr uint16_t BASE = 0x3000U, SLOT_BYTES = 1536U;
constexpr uint8_t CAPACITY = 16U;
constexpr uint32_t END = static_cast<uint32_t>(BASE) + CAPACITY * 2UL * SLOT_BYTES;
constexpr uint32_t MAGIC = 0x4D414E31UL;
#pragma pack(push, 1)
struct Note {
  char id[37]{};
  uint32_t version = 0;
  uint64_t createdAt = 0;
  uint8_t type = 2; // 1=batch, 2=machine
  char title[241]{}, content[1201]{}; // UTF-8, at most 60/300 UTF-16 units.
};
struct Record { uint32_t magic; uint16_t schema; uint8_t live, reserved; Note note; uint32_t crc; };
#pragma pack(pop)
static_assert(sizeof(Record) <= SLOT_BYTES, "Notes record exceeds reserved bank");
enum class Code : uint8_t { Ok, Eeprom, Corrupt, Full, Conflict, Invalid, NotFound };
enum class Operation : uint8_t { List, Upsert, Remove };
enum class IoStage : uint8_t { None, Write, Readback, Verify };
inline const char *stageText(IoStage stage) {
  switch(stage){case IoStage::Write:return "WRITE";case IoStage::Readback:return "READBACK";case IoStage::Verify:return "VERIFY";default:return "NONE";}
}
struct Request { uint32_t token = 0; Operation operation = Operation::List; uint8_t cursor = 0; bool allowBatch = false, reconcile = false; Note note{}; };
struct Response { uint32_t token = 0; Code code = Code::Ok; uint8_t nextCursor = 0; bool done = true, hasNote = false, ambiguous = false; IoStage ioStage = IoStage::None; uint16_t ioAddress = 0; int16_t mismatch = -1; bool blank = false, validReadback = false; uint16_t failedAddress=0; uint8_t writeReason=0, requested=0, written=0, wireError=0; Note note{}; };
inline uint32_t crc32(const void *data, size_t size) {
  uint32_t crc = 0xFFFFFFFFUL; const auto *p = static_cast<const uint8_t *>(data);
  while (size--) { crc ^= *p++; for (uint8_t bit=0;bit<8;++bit) crc=(crc>>1)^((crc&1U)?0xEDB88320UL:0U); }
  return ~crc;
}
inline bool validId(const char *id) {
  if (!id || strnlen(id,37)!=36) return false;
  for (uint8_t i=0;i<36;++i) {
    if (i==8 || i==13 || i==18 || i==23) { if (id[i]!='-') return false; }
    else if (!((id[i]>='0'&&id[i]<='9') || (id[i]>='a'&&id[i]<='f'))) return false;
  } return true;
}
inline bool whitespace(uint32_t c) {
  return c==9 || c==10 || c==13 || c==32 || c==0x85 || c==0xA0 || c==0x1680 ||
    (c>=0x2000&&c<=0x200A) || c==0x2028 || c==0x2029 || c==0x202F || c==0x205F || c==0x3000 || c==0xFEFF;
}
inline bool validText(const char *text,size_t capacity,uint16_t maxUnits,bool required) {
  const size_t size=strnlen(text,capacity); if(size==capacity) return false;
  uint16_t units=0; bool meaningful=false;
  for(size_t i=0;i<size;) {
    const uint8_t first=static_cast<uint8_t>(text[i++]); uint32_t c=first; uint8_t more=0; uint32_t minimum=0;
    if(first>=0xC2&&first<=0xDF){more=1;c=first&31U;minimum=0x80;}
    else if(first>=0xE0&&first<=0xEF){more=2;c=first&15U;minimum=0x800;}
    else if(first>=0xF0&&first<=0xF4){more=3;c=first&7U;minimum=0x10000;}
    else if(first>=0x80) return false;
    if(i+more>size) return false;
    while(more--){const uint8_t next=static_cast<uint8_t>(text[i++]);if((next&0xC0U)!=0x80U)return false;c=(c<<6)|(next&63U);}
    if(c<minimum || c>0x10FFFF || (c>=0xD800&&c<=0xDFFF) || (c<32 && c!=9&&c!=10&&c!=13) || c==127) return false;
    units+=c>0xFFFF?2:1; if(units>maxUnits)return false; meaningful|=!whitespace(c);
  }
  return !required||meaningful;
}
inline bool validNote(const Note &n) {
  return validId(n.id) && n.createdAt>0 && n.createdAt<=8640000000000000ULL && (n.type==1 || n.type==2) &&
    validText(n.title,sizeof(n.title),60,false) && validText(n.content,sizeof(n.content),300,true);
}
inline const char *codeText(Code code) {
  switch(code) {
    case Code::Ok:return "NOTES_STORED";case Code::Eeprom:return "NOTES_EEPROM_ERROR";
    case Code::Corrupt:return "NOTES_CORRUPT";case Code::Full:return "NOTES_FULL";
    case Code::Conflict:return "NOTES_CONFLICT";case Code::Invalid:return "NOTES_INVALID";
    case Code::NotFound:return "NOTES_NOT_FOUND";
  } return "NOTES_INVALID";
}
template<class Eeprom> class Store {
  struct Index { char id[37]{}; uint32_t version=0; bool live=false; uint8_t bank=1; };
  Eeprom &io_; Index index_[CAPACITY]{}; Record a_{}, b_{}; bool loaded_=false;
  static uint16_t address(uint8_t slot,uint8_t bank) { return BASE+(slot*2U+bank)*SLOT_BYTES; }
  static bool valid(const Record &r) {
    return r.magic==MAGIC && r.schema==1 && r.reserved==0 && r.live<=1 && r.note.version && validNote(r.note) && r.crc==crc32(&r,offsetof(Record,crc));
  }
  Code read(uint8_t slot,uint8_t bank,Record &r,bool &blank) {
    uint8_t head[8]; if(!io_.readBytes(address(slot,bank),head,sizeof(head)))return Code::Eeprom;
    blank=true; for(uint8_t byte:head) if(byte!=0xFF)blank=false;
    if(blank){r=Record{};return Code::Ok;}
    return io_.readBytes(address(slot,bank),&r,sizeof(r))?Code::Ok:Code::Eeprom;
  }
  Code initialize() {
    if(loaded_)return Code::Ok;
    for(uint8_t i=0;i<CAPACITY;++i) {
      bool ba=false,bb=false; if(read(i,0,a_,ba)!=Code::Ok || read(i,1,b_,bb)!=Code::Ok)return Code::Eeprom;
      const bool va=valid(a_),vb=valid(b_);
      if(!va&&!vb&&!ba&&!bb)return Code::Corrupt;
      index_[i]=Index{};
      if(!va&&!vb)continue; // Blank peer means an interrupted first, unacknowledged write.
      if(va&&vb&&a_.note.version==b_.note.version&&memcmp(&a_,&b_,sizeof(a_)))return Code::Corrupt;
      const bool chooseA=va&&(!vb || static_cast<int32_t>(a_.note.version-b_.note.version)>0);
      const Record &r=chooseA?a_:b_; index_[i].bank=chooseA?0:1;index_[i].version=r.note.version;index_[i].live=r.live;
      memcpy(index_[i].id,r.note.id,sizeof(r.note.id));
    }
    for(uint8_t i=0;i<CAPACITY;++i)for(uint8_t j=i+1;j<CAPACITY;++j)
      if(index_[i].live&&index_[j].live&&!strcmp(index_[i].id,index_[j].id))return Code::Corrupt;
    loaded_=true;return Code::Ok;
  }
  Code selected(uint8_t slot) {
    bool blank=false;const Code result=read(slot,index_[slot].bank,a_,blank);
    if(result!=Code::Ok)return result;
    return !blank&&valid(a_)&&a_.note.version==index_[slot].version&&!strcmp(a_.note.id,index_[slot].id)?Code::Ok:Code::Corrupt;
  }
 public:
  explicit Store(Eeprom &io):io_(io){}
  Response process(const Request &request) {
    Response out{};out.token=request.token;out.ambiguous=request.reconcile;
    out.code=initialize();if(out.code!=Code::Ok)return out;
    if(request.operation==Operation::List) {
      if(request.cursor>CAPACITY){out.code=Code::Invalid;return out;}
      uint8_t slot=request.cursor;
      while(slot<CAPACITY&&!index_[slot].live)++slot;
      out.nextCursor=slot<CAPACITY?slot+1:CAPACITY;out.done=out.nextCursor==CAPACITY;
      if(slot<CAPACITY){out.code=selected(slot);if(out.code==Code::Ok){out.note=a_.note;out.hasNote=true;}}
      return out;
    }
    if(!validId(request.note.id) || (request.operation==Operation::Upsert&&!validNote(request.note))){out.code=Code::Invalid;return out;}
    uint8_t slot=CAPACITY;
    for(uint8_t i=0;i<CAPACITY;++i)if(!strcmp(index_[i].id,request.note.id)){slot=i;break;}
    const bool existing=slot<CAPACITY;
    if(existing && request.reconcile) {
      out.code=selected(slot);if(out.code!=Code::Ok)return out;
      const uint32_t next=request.note.version==UINT32_MAX?1:request.note.version+1;
      Note candidate=request.note;candidate.version=a_.note.version;if(request.note.version)candidate.createdAt=a_.note.createdAt;
      if((request.operation==Operation::Remove&&!index_[slot].live&&a_.note.version==next) ||
         (request.operation==Operation::Upsert&&index_[slot].live&&
          (!request.note.version||a_.note.version==next)&&!memcmp(&candidate,&a_.note,sizeof(Note)))) {
        out.note=a_.note;out.hasNote=index_[slot].live;out.ambiguous=false;return out;
      }
    }
    if(existing && (!index_[slot].live || request.note.version!=index_[slot].version)){out.code=Code::Conflict;return out;}
    if(!existing && (request.note.version || request.operation==Operation::Remove)){out.code=Code::NotFound;return out;}
    if(existing){out.code=selected(slot);if(out.code!=Code::Ok)return out;}
    if(request.operation==Operation::Upsert && request.note.type==1&&!request.allowBatch&&(!existing||a_.note.type!=1)){out.code=Code::Invalid;return out;}
    if(!existing){for(uint8_t i=0;i<CAPACITY;++i)if(!index_[i].live){slot=i;break;}if(slot==CAPACITY){out.code=Code::Full;return out;}}
    // No-op edit avoids EEPROM wear and returns the existing durable version.
    Note candidate=request.operation==Operation::Remove?a_.note:request.note;
    if(existing)candidate.createdAt=a_.note.createdAt;
    if(existing&&request.operation==Operation::Upsert&&!memcmp(&candidate,&a_.note,sizeof(Note))){out.note=a_.note;out.hasNote=true;out.ambiguous=false;return out;}
    b_=Record{};b_.magic=MAGIC;b_.schema=1;b_.live=request.operation==Operation::Upsert;
    b_.note=candidate;b_.note.version=index_[slot].version+1U;if(!b_.note.version)b_.note.version=1;
    b_.crc=crc32(&b_,offsetof(Record,crc));const uint8_t bank=1U-index_[slot].bank;
    out.ambiguous=true;out.ioAddress=address(slot,bank);out.ioStage=IoStage::Write;
    if(!io_.writeBytes(address(slot,bank),&b_,sizeof(b_))){loaded_=false;out.code=Code::Eeprom;return out;}
    out.ioStage=IoStage::Readback;
    bool blank=false;out.code=read(slot,bank,a_,blank);
    if(out.code!=Code::Ok){loaded_=false;return out;}
    out.ioStage=IoStage::Verify;out.blank=blank;out.validReadback=!blank&&valid(a_);
    if(!out.validReadback || memcmp(&a_,&b_,sizeof(a_))){
      const auto *actual=reinterpret_cast<const uint8_t *>(&a_),*expected=reinterpret_cast<const uint8_t *>(&b_);
      for(size_t i=0;i<sizeof(a_);++i)if(actual[i]!=expected[i]){out.mismatch=static_cast<int16_t>(i);break;}
      loaded_=false;out.code=Code::Eeprom;return out;
    }
    index_[slot].bank=bank;index_[slot].live=b_.live;index_[slot].version=b_.note.version;memcpy(index_[slot].id,b_.note.id,sizeof(b_.note.id));
    out.note=a_.note;out.hasNote=b_.live;out.ambiguous=false;out.ioStage=IoStage::None;return out;
  }
};
} // namespace MayapNotes
// Implemented by the isolated storage owner, not the control or realtime task.
bool mayapNotesStart();
bool mayapNotesSubmit(const MayapNotes::Request &request);
bool mayapNotesReceive(MayapNotes::Response &response);
