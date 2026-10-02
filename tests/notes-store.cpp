#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
static uint32_t clockMs = 0;
uint32_t millis() { return clockMs; }
uint32_t elapsedMs(uint32_t now, uint32_t before) { return now - before; }
uint32_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
void vTaskDelay(uint32_t ms) { clockMs += ms; }
static bool lockAvailable=true;
bool mayapI2cLock(uint32_t) { return lockAvailable; }
void mayapI2cUnlock() {}
void mayapI2cReport(uint8_t, bool) {}
int constrain(int value, int minimum, int maximum) {
  return std::max(minimum, std::min(value, maximum));
}
struct FakeWire {
  std::array<uint8_t, 65536> memory{};
  std::vector<uint8_t> tx, rx;
  size_t cursor = 0, writes = 0;
  uint16_t word = 0;
  uint8_t chip = 0;
  int cutAfter = -1;
  bool off = false, wp = false; bool failReadAfterWrite = false, busyProbe=false; size_t txLimit=128;
  FakeWire() { memory.fill(0xFF); }
  void beginTransmission(uint8_t address) { chip = address; tx.clear(); }
  size_t write(uint8_t byte) {
    if (tx.size() >= txLimit) return 0;
    tx.push_back(byte); return 1;
  }
  size_t write(const uint8_t *data, size_t length) {
    size_t n = 0;
    while (n < length && write(data[n])) ++n;
    return n;
  }
  uint8_t endTransmission(bool) {
    if (off || chip != 0x50 || (busyProbe&&writes&&tx.empty())) return 2;
    if (tx.size() >= 2) word = static_cast<uint16_t>((tx[0] << 8) | tx[1]);
    if (tx.size() > 2) {
      ++writes;
      for (size_t i = 2; i < tx.size(); ++i) {
        if (cutAfter == 0) { off = true; return 2; }
        if (cutAfter > 0) --cutAfter;
        // Actual C512 page wrap. Incorrect driver geometry corrupts readback.
        const uint16_t address = static_cast<uint16_t>((word & 0xFF80) |
                                                   ((word + i - 2) & 0x7F));
        if (!wp) memory[address] = tx[i];
      }
    }
    return 0;
  }
  size_t requestFrom(uint8_t address, uint8_t length, bool) {
    rx.clear(); cursor = 0;
    if (failReadAfterWrite && writes) return 0;
    if (off || address != 0x50) return 0;
    for (uint8_t i = 0; i < length; ++i) rx.push_back(memory[word++]);
    return rx.size();
  }
  int available() { return static_cast<int>(rx.size() - cursor); }
  int read() { return available() ? rx[cursor++] : -1; }
} Wire;
#include "actual-notes-driver.inc"
#include "notes_store.h"
using namespace MayapNotes;
Request save(unsigned number=1) {
  Request r;r.operation=Operation::Upsert;r.token=number;
  snprintf(r.note.id,sizeof(r.note.id),"00000000-0000-4000-8000-%012u",number);
  r.note.createdAt=1790000000000ULL;
  strcpy(r.note.title,"Soi trứng ngày 7");strcpy(r.note.content,"Loại 12 quả không phát triển.");return r;
}
Response list(Store<ExternalEeprom24xx> &store,uint8_t cursor=0) {Request r;r.cursor=cursor;return store.process(r);}
int main() {
  ExternalEeprom24xx io;Store<ExternalEeprom24xx> store(io);
  Wire.memory.fill(0xFF);
  // Known config/reminder/history bytes and unused upper area must survive.
  for(size_t i=0;i<BASE;++i)Wire.memory[i]=static_cast<uint8_t>(i);
  for(size_t i=END;i<Wire.memory.size();++i)Wire.memory[i]=static_cast<uint8_t>(i);
  auto request=save();auto result=store.process(request);assert(result.code==Code::Ok&&result.note.version==1);
  const auto committed=Wire.memory;const size_t written=Wire.writes;
  request.note.version=1;result=store.process(request);assert(result.code==Code::Ok&&Wire.writes==written);
  request.note.version=0;assert(store.process(request).code==Code::Conflict);request.note.version=1;
  strcpy(request.note.content,"Cập nhật ghi chú.");request.note.createdAt+=1000;
  result=store.process(request);assert(result.code==Code::Ok&&result.note.version==2&&result.note.createdAt==1790000000000ULL);
  assert(store.process(request).code==Code::Conflict);request.note=result.note;
  Store<ExternalEeprom24xx> reboot(io);assert(!strcmp(list(reboot).note.content,"Cập nhật ghi chú."));
  const auto beforeEdit=Wire.memory;strcpy(request.note.content,"Sau khi ghi lần ba.");
  for(int cut=0;cut<=static_cast<int>(sizeof(Record));++cut) {
    Wire.memory=beforeEdit;Wire.off=false;Wire.cutAfter=cut;Store<ExternalEeprom24xx> power(io);
    const auto attempt=power.process(request);if(cut<static_cast<int>(sizeof(Record)))assert(attempt.code==Code::Eeprom&&attempt.ambiguous);
    Wire.off=false;Wire.cutAfter=-1;Store<ExternalEeprom24xx> after(io);const auto recovered=list(after);
    assert(recovered.code==Code::Ok&&recovered.hasNote);
    assert(!strcmp(recovered.note.content,"Cập nhật ghi chú.")||!strcmp(recovered.note.content,"Sau khi ghi lần ba."));
    if(attempt.code==Code::Ok)assert(!strcmp(recovered.note.content,"Sau khi ghi lần ba."));
    for(size_t i=0;i<BASE;++i)assert(Wire.memory[i]==static_cast<uint8_t>(i));
    for(size_t i=END;i<Wire.memory.size();++i)assert(Wire.memory[i]==static_cast<uint8_t>(i));
  }
  Wire.memory=committed;Wire.off=false;Wire.cutAfter=-1;Wire.writes=0;
  Store<ExternalEeprom24xx> ambiguous(io);request=save(2);Wire.failReadAfterWrite=true;
  result=ambiguous.process(request);assert(result.code==Code::Eeprom&&result.ambiguous&&result.ioStage==IoStage::Readback);
  const size_t once=Wire.writes;Wire.failReadAfterWrite=false;request.reconcile=true;
  result=ambiguous.process(request);assert(result.code==Code::Ok&&!result.ambiguous&&result.note.version==1&&Wire.writes==once);
  // Delete ambiguity likewise reconciles the tombstone without a second write.
  Request remove;remove.operation=Operation::Remove;remove.note=result.note;
  Wire.writes=0;Wire.failReadAfterWrite=true;result=ambiguous.process(remove);assert(result.ambiguous);
  const size_t removed=Wire.writes;Wire.failReadAfterWrite=false;remove.reconcile=true;
  result=ambiguous.process(remove);assert(result.code==Code::Ok&&!result.ambiguous&&!result.hasNote&&Wire.writes==removed);
  for(unsigned i=2;i<=CAPACITY;++i){auto next=save(i+1);assert(ambiguous.process(next).code==Code::Ok);}
  assert(ambiguous.process(save(99)).code==Code::Full);
  Store<ExternalEeprom24xx> fullReboot(io);uint8_t cursor=0;unsigned count=0;
  do{result=list(fullReboot,cursor);assert(result.code==Code::Ok);count+=result.hasNote;cursor=result.nextCursor;}while(!result.done);
  assert(count==CAPACITY);
  auto batch=save(100);batch.note.type=1;assert(fullReboot.process(batch).code==Code::Invalid);
  auto invalid=save(100);memset(invalid.note.content,'x',301);invalid.note.content[301]=0;assert(fullReboot.process(invalid).code==Code::Invalid);
  strcpy(invalid.note.content,"　 \n\t");assert(fullReboot.process(invalid).code==Code::Invalid);
  strcpy(invalid.note.content,"\xED\xA0\x80");assert(fullReboot.process(invalid).code==Code::Invalid);
  // Write protection can return successful I2C yet must fail readback.
  Wire.memory=committed;Wire.wp=true;Store<ExternalEeprom24xx> protectedStore(io);
  const auto wpResult=protectedStore.process(save(100));
  assert(wpResult.code==Code::Eeprom&&wpResult.ioStage==IoStage::Verify&&wpResult.blank&&wpResult.mismatch==0&&!wpResult.validReadback);Wire.wp=false;
  Wire.memory=committed;Wire.cutAfter=0;Store<ExternalEeprom24xx> writeFailure(io);
  const auto writeResult=writeFailure.process(save(100));assert(writeResult.ioStage==IoStage::Write&&writeResult.ambiguous);
  assert(io.lastWriteTrace().reason==3&&io.lastWriteTrace().error==2&&io.lastWriteTrace().requested==126);
  Wire.off=false;Wire.cutAfter=-1;
  // CRC fallback to the older bank, and explicit error if both copies corrupt.
  Wire.memory=beforeEdit;Wire.memory[BASE+SLOT_BYTES+offsetof(Record,note)+100]^=1;
  Store<ExternalEeprom24xx> fallback(io);assert(list(fallback).note.version==1);
  Wire.memory[BASE+offsetof(Record,note)+100]^=1;Store<ExternalEeprom24xx> corrupt(io);assert(list(corrupt).code==Code::Corrupt);
  uint8_t payload[126]{};
  lockAvailable=false;assert(!io.writeBytes(BASE,payload,sizeof(payload)));assert(io.lastWriteTrace().reason==1);lockAvailable=true;
  Wire.txLimit=32;assert(!io.writeBytes(BASE,payload,sizeof(payload)));assert(io.lastWriteTrace().reason==2&&io.lastWriteTrace().written==30);Wire.txLimit=128;
  Wire.writes=0;Wire.busyProbe=true;assert(!io.writeBytes(BASE,payload,sizeof(payload)));assert(io.lastWriteTrace().reason==4&&io.lastWriteTrace().error==0);Wire.busyProbe=false;
  assert(io.writeBytes(BASE,payload,sizeof(payload)));assert(io.lastWriteTrace().reason==0);
  printf("Notes AT24C512: CRUD/reboot/version/conflict/full/UTF-8/WP/CRC; %zu power-cut byte positions; isolated memory and uncertain reconciliation PASS\n",sizeof(Record)+1);
}
