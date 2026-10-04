#include <cassert>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include "../MAYAP_INDUSTRIAL_v1_0_0/note_journal.h"
using namespace MayapNoteJournal;
struct Cut {};
struct Memory {
  std::vector<uint8_t> bytes=std::vector<uint8_t>(65536,0xFF);
  int budget=-1,calls=0,writeCalls=0;bool falseAck=false,protectedWrite=false,failedRead=false;
  bool readBytes(uint16_t a,void *p,size_t n){++calls;assert(n<=32);if(failedRead)return false;memcpy(p,&bytes[a],n);return true;}
  bool writeBytes(uint16_t a,const void *p,size_t n){++calls;++writeCalls;assert(a>=Base&&a+n<=End&&n<=32&&(a%128)+n<=128);
    if(protectedWrite)return false;
    for(size_t i=0;i<n;++i){if(budget==0)throw Cut{};if(budget>0)--budget;bytes[a+i]=static_cast<const uint8_t *>(p)[i];}
    return !falseAck;
  }
};
template<class J> Result run(J &j,Memory &m,const Request &r){assert(j.begin(r));for(int i=0;i<10000&&!j.ready();++i){m.calls=0;j.step();assert(m.calls<=1);}assert(j.ready());return j.result();}
Request save(unsigned id,uint32_t g,uint32_t version=0,const char *text="Ghi chú tiếng Việt: soi trứng ngày 7"){
  Request r;r.operation=Operation::Save;r.generation=g;r.note.version=version;r.note.createdAt=1750000000000ULL;
  snprintf(r.note.id,sizeof(r.note.id),"00000000-0000-4000-8000-%012x",id);strcpy(r.note.content,text);return r;
}
static std::vector<Document> list(Journal<Memory>&j,Memory&m,uint32_t &g){
  std::vector<Document> notes;Request r;for(int i=0;i<=Slots;++i){auto out=run(j,m,r);assert(out.code==Code::Ok);
    g=out.generation;if(out.hasNote)notes.push_back(j.document());if(out.done)return notes;
    r.cursor=out.next;r.generation=g;r.snapshot=true;}
  assert(false);return notes;
}
int main(){
  Memory m;Journal<Memory> j(m);uint32_t g;assert(list(j,m,g).empty()&&g==0);
  auto r=save(1,0);assert(run(j,m,r).code==Code::Ok);assert(j.document().version==1);
  m.falseAck=true;r=save(1,1,1,"Không báo lỗi giả khi ghi xong nhưng ACK polling trả false");assert(run(j,m,r).code==Code::Ok);m.falseAck=false;
  auto committed=m.bytes;uint32_t oldG=2;
  // All 1576 byte boundaries: invalidation + payload + commit seal.
  const int writes=32+sizeof(Record)+32;
  for(int cut=0;cut<=writes;++cut){Memory damaged;damaged.bytes=committed;damaged.budget=cut;Journal<Memory> writer(damaged);
    try{run(writer,damaged,save(1,oldG,oldG,"NEW complete journal document"));}catch(const Cut&){}
    damaged.budget=-1;Journal<Memory> reboot(damaged);auto notes=list(reboot,damaged,g);assert(notes.size()==1);
    assert(notes[0].version==oldG||notes[0].version==oldG+1);
    if(notes[0].version==oldG+1)assert(!strcmp(notes[0].content,"NEW complete journal document"));
    // Config/reminders/history and tail reservation are byte-identical.
    assert(std::equal(committed.begin(),committed.begin()+Base,damaged.bytes.begin()));
    assert(std::equal(committed.begin()+End,committed.end(),damaged.bytes.begin()+End));
  }
  // Tombstone is atomic too: power loss exposes the old note or absence.
  for(int cut=0;cut<=writes;++cut){Memory damaged;damaged.bytes=committed;damaged.budget=cut;Journal<Memory> writer(damaged);
    Request del;del.operation=Operation::Remove;del.generation=oldG;del.note=save(1,oldG,oldG).note;
    try{run(writer,damaged,del);}catch(const Cut&){}
    damaged.budget=-1;Journal<Memory> reboot(damaged);auto notes=list(reboot,damaged,g);
    assert(notes.empty()||(notes.size()==1&&notes[0].version==oldG));
  }
  // Fill, edit past wrap, delete/recreate with distinct IDs and reboot often.
  for(unsigned id=2;id<=Capacity;++id){auto out=run(j,m,save(id,oldG));assert(out.code==Code::Ok);oldG=out.generation;}
  assert(run(j,m,save(100,oldG)).code==Code::Full);
  Request reminders;reminders.operation=Operation::SaveReminders;reminders.generation=oldG;
  strcpy(reminders.note.id,ReminderId);reminders.note.createdAt=1;
  ReminderPayload payload;for(unsigned i=0;i<10;++i){payload.items[i].day=i+1;strcpy(payload.items[i].label,"Nhắc nhở tiếng Việt");}
  memcpy(reminders.note.content,&payload,sizeof(payload));auto reminderResult=run(j,m,reminders);
  assert(reminderResult.code==Code::Ok);oldG=reminderResult.generation;
  auto reminderBaseline=m.bytes;reminders.generation=oldG;reminders.note.version=oldG;
  payload.items[0].day=20;memcpy(reminders.note.content,&payload,sizeof(payload));
  for(int cut=0;cut<=writes;++cut){Memory damaged;damaged.bytes=reminderBaseline;damaged.budget=cut;Journal<Memory> writer(damaged);
    try{run(writer,damaged,reminders);}catch(const Cut&){}
    damaged.budget=-1;Journal<Memory> restored(damaged);Request read;read.operation=Operation::ReadReminders;strcpy(read.note.id,ReminderId);
    auto out=run(restored,damaged,read);assert(out.code==Code::Ok&&out.hasNote);
    ReminderPayload loaded;memcpy(&loaded,restored.document().content,sizeof(loaded));assert(loaded.items[0].day==1||loaded.items[0].day==20);
    assert(loaded.items[9].day==10);assert(list(restored,damaged,g).size()==Capacity);
  }
  for(unsigned cycle=0;cycle<100;++cycle){Journal<Memory> reboot(m);auto notes=list(reboot,m,g);assert(notes.size()==Capacity);
    Request del;del.operation=Operation::Remove;del.generation=g;del.note=notes.front();auto out=run(reboot,m,del);assert(out.code==Code::Ok);
    const int beforeRetry=m.writeCalls;assert(run(reboot,m,del).code==Code::Ok&&m.writeCalls==beforeRetry);
    auto stale=save(200+cycle,g);assert(run(reboot,m,stale).code==Code::Conflict);
    out=run(reboot,m,save(200+cycle,out.generation));assert(out.code==Code::Ok);
  }
  // Many mutations without reboot must preserve tombstones until old copies
  // are physically reclaimed; dropping a tombstone index could resurrect a note.
  { Memory continuous;Journal<Memory> writer(continuous);uint32_t seq=0;
    for(unsigned id=1;id<=Capacity;++id){auto out=run(writer,continuous,save(id,seq));assert(out.code==Code::Ok);seq=out.generation;}
    for(unsigned cycle=0;cycle<100;++cycle){auto current=list(writer,continuous,seq);assert(current.size()==Capacity);
      Request del;del.operation=Operation::Remove;del.generation=seq;del.note=current.front();auto out=run(writer,continuous,del);assert(out.code==Code::Ok);
      out=run(writer,continuous,save(1000+cycle,out.generation));assert(out.code==Code::Ok);seq=out.generation;
      Journal<Memory> check(continuous);auto mounted=list(check,continuous,g);assert(mounted.size()==Capacity);
      assert(std::none_of(mounted.begin(),mounted.end(),[&](const Document &n){return !strcmp(n.id,del.note.id);}));
    }
  }
  Journal<Memory> reboot(m);auto notes=list(reboot,m,g);assert(notes.size()==Capacity);
  Request same=save(0,g-1);same.note=notes[0];assert(run(reboot,m,same).code==Code::Ok); // verified no-op
  m.protectedWrite=true;auto failure=save(0,g,notes[0].version,"WP protected write");failure.note.id[0]=notes[0].id[0];strcpy(failure.note.id,notes[0].id);
  assert(run(reboot,m,failure).code==Code::Io);m.protectedWrite=false;
  Journal<Memory> afterFail(m);assert(list(afterFail,m,g).size()==Capacity);
  m.failedRead=true;assert(run(afterFail,m,Request{}).code==Code::Io);m.failedRead=false;
  auto invalid=save(77,g);strcpy(invalid.note.content," \t\n\xC2\xA0");assert(run(afterFail,m,invalid).code==Code::Invalid);
  assert(!textValid("\xC0\xAF",3,300,true));assert(!textValid("\xED\xA0\x80",4,300,true));
  std::puts("Shared journal: 1577 power-cut positions each for notes, tombstones and 10 reminders, tiny page I/O, false write-ACK recovery, 100 reclaim/reboots, revision/no-op/UTF8/WP/failure isolation PASS");
}
