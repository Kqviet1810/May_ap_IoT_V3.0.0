#pragma once
#include "thermal_observer.h"
#include <cstddef>
#include <cstring>
namespace MayapAdaptive {
#pragma pack(push,1)
struct Model {
  uint32_t magic,version,signature,epoch,sequence;
  float load,coast,coastSec,hold,confidence;
  uint32_t windows,crc;
};
#pragma pack(pop)
inline uint32_t modelCrc(const Model &model){
  const uint8_t *p=reinterpret_cast<const uint8_t *>(&model);uint32_t c=0xffffffff;
  for(size_t n=0;n<offsetof(Model,crc);++n){c^=p[n];for(unsigned b=0;b<8;++b)c=(c>>1)^(0xedb88320U&(0U-(c&1U)));}
  return ~c;
}
inline bool validModel(const Model &m){return m.magic==0x41544231&&m.version==1&&m.crc==modelCrc(m)&&
  ThermalObserver::finiteSeed(m.load,m.coast,m.coastSec,m.hold)&&std::isfinite(m.confidence)&&m.confidence>=0&&m.confidence<=100;}
inline bool compatibleModel(const Model &m,uint32_t signature,uint32_t epoch,bool abnormal){return
  !abnormal&&validModel(m)&&m.signature==signature&&m.epoch>0&&epoch>=m.epoch&&epoch-m.epoch<=Policy::ModelMaxAgeSec;}
inline Model makeModel(const Estimates &e,uint32_t signature,uint32_t epoch,uint32_t sequence){
  Model m{};m.magic=0x41544231;m.version=1;m.signature=signature;m.epoch=epoch;m.sequence=sequence;
  m.load=e.load;m.coast=e.coast;m.coastSec=e.coastSec;m.hold=e.hold;m.confidence=e.confidence;m.windows=e.windows;
  m.crc=modelCrc(m);return m;
}
}
