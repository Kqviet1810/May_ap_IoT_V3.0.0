#include "thermal-fixture.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_control.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/heater_burst_scheduler.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/sensor_format.h"
namespace Old {
#include "fixtures/thermal-v1/thermal_control.h"
#include "fixtures/thermal-v1/ssr_window.h"
}

static void sensorFormats() {
  const uint16_t registers[] = {375U, 3751U, 30902U};
  for (unsigned i = 0; i < 3; ++i) {
    SensorFormatDecoder decoder;
    for (unsigned n = 0; n < 5; ++n) {
      assert(!decoder.accept(registers[i], 600));
      assert(!decoder.locked() && !decoder.valid());
    }
    assert(decoder.accept(registers[i], 600));
    assert(static_cast<unsigned>(decoder.format()) == i + 1);
    const float expected = i == 0 ? 37.5f : i == 1 ? 37.51f : -45.0f + 175.0f * registers[i] / 65535.0f;
    assert(std::fabs(decoder.temperature() - expected) < 0.00001f);
    const auto locked = decoder.format();
    // A register scale change never starts a fresh detector after lock.
    const uint16_t wrong = i == 0 ? 3751U : 375U;
    assert(!decoder.accept(wrong, 600) && !decoder.valid());
    assert(decoder.format() == locked);
    for (unsigned n = 0; n < 20; ++n) assert(!decoder.accept(wrong, 600));
    assert(decoder.accept(registers[i], 600) && decoder.format() == locked);
    decoder.rejectSample(); // Rejected measurement: invalidate but keep format.
    assert(!decoder.valid() && decoder.locked());
    assert(decoder.accept(registers[i], 600));
    const uint16_t hotRegisters[]={600U,6000U,39321U};
    assert(decoder.accept(hotRegisters[i],600));
    assert(decoder.valid() && decoder.temperature()>59.99f); // Emergency must see upward raw jumps.
  }
  SensorFormatDecoder interrupted;
  for (unsigned n = 0; n < 5; ++n) interrupted.accept(375,600);
  interrupted.rejectSample();
  for (unsigned n = 0; n < 5; ++n) assert(!interrupted.accept(375,600));
  assert(interrupted.accept(375,600));
  SensorFormatDecoder invalid;
  for (unsigned n = 0; n < 50; ++n) {
    assert(!invalid.accept(0, 600)); // Ambiguous/implausible cold AUTO boot => no heat.
    assert(!invalid.accept(65000, 600));
    assert(!invalid.accept(375, 1001));
  }
  assert(!invalid.locked());
  SensorFormatDecoder explicitCold(SensorTemperatureFormat::TempX100);
  for (unsigned n = 0; n < 5; ++n) assert(!explicitCold.accept(500,600));
  assert(explicitCold.accept(500,600) && std::fabs(explicitCold.temperature()-5.0f)<0.001f);
  SensorFormatDecoder nativeRH(SensorTemperatureFormat::Sht30Raw16, true);
  for (unsigned n = 0; n < 6; ++n) nativeRH.accept(30902,39321);
  assert(nativeRH.valid() && std::fabs(nativeRH.humidity()-60.0f)<0.001f);
}

static void pidWeightsAndPermits() {
  for (float beta : {1.0f,0.7f,0.5f,0.35f}) {
    MachineConfig cfg;
    cfg.kp=10; cfg.ki=0; cfg.kd=100;
    ThermalController pid(beta);
    assert(std::fabs(pid.updateOnNewSample(1000,1,0,cfg,true)-10*beta)<0.001f);
    const float stepped=pid.updateOnNewSample(3000,3,0,cfg,true);
    assert(std::fabs(stepped-30*beta)<0.001f); // P weighting; D has no SP kick.
    cfg.kp=12;
    pid.applyConfigBumpless(3000,3,0,cfg);
    assert(std::fabs(pid.output()-stepped)<0.001f);
    assert(std::fabs(pid.updateOnNewSample(5000,3,0,cfg,true)-stepped)<0.001f);
    cfg.kp=0; cfg.ki=1; cfg.kd=0;
    pid.reset();
    pid.updateOnNewSample(1000,35,34,cfg,true);
    assert(std::fabs(pid.updateOnNewSample(3000,35,34,cfg,true)-2)<0.001f);
    assert(std::fabs(pid.updateOnNewSample(3000,35,34,cfg,true)-2)<0.001f); // no duplicate integration
    assert(std::fabs(pid.updateOnNewSample(6000,35,34,cfg,true)-5)<0.001f); // real 3 s dt
    // Safety cut, sensor fault, high temp, vent and permit loss share disabled reset.
    for (unsigned reason = 0; reason < 6; ++reason) {
      assert(pid.updateOnNewSample(8000,35,34,cfg,false)==0);
      assert(pid.updateOnNewSample(10000,35,34,cfg,true)==0);
    }
    assert(pid.updateOnNewSample(12000,35,NAN,cfg,true)==0);
    cfg.kp=1; cfg.ki=1; cfg.maxHeaterPower=17;
    pid.reset();
    for (uint32_t t=1000; t<3600000; t+=2000)
      assert(pid.updateOnNewSample(t,100,0,cfg,true)==17);
    assert(pid.updateOnNewSample(3600000,100,100,cfg,true)<0.00001f); // no high-saturation windup
    pid.reset();
    for (uint32_t t=1000; t<3600000; t+=2000)
      assert(pid.updateOnNewSample(t,20,30,cfg,true)==0);
    assert(pid.updateOnNewSample(3600000,20,20,cfg,true)<0.00001f); // no low-saturation windup
  }
}

static void burstEnergy() {
  for (unsigned groups : {1U,2U}) {
    for (float power : {0.5f,1.f,2.f,3.f,5.f,10.f,25.f,50.f,75.f,100.f}) {
      for (unsigned seconds : {120U,600U,3600U}) {
        HeaterBurstScheduler scheduler(groups);
        unsigned a=0,b=0;
        for (unsigned s=0; s<seconds; ++s) {
          auto d=scheduler.update(s*1000U,power,true);
          a+=d.groupA; b+=d.groupB;
          if (power<=50 && groups==2) assert(!(d.groupA && d.groupB));
          if (groups==1) assert(!d.groupB);
        }
        const double requested=seconds*power*groups/100.0;
        assert(a+b<=requested+1e-7 && requested-(a+b)<1.000001);
        if (groups==2) assert(std::abs(static_cast<int>(a)-static_cast<int>(b))<=1);
        std::printf("LOW_DUTY groups=%u request=%.1f seconds=%u delivered=%.6f\n",groups,power,seconds,100.0*(a+b)/(seconds*groups));
      }
    }
  }
  HeaterBurstScheduler scheduler(2);
  assert(scheduler.update(0,100,true).groupA);
  auto d=scheduler.update(1,100,false); // OFF immediately, not at quantum boundary.
  assert(!d.groupA && !d.groupB);
  d=scheduler.update(2,0.5f,true); assert(!d.groupA && !d.groupB);
  for (unsigned s=1; s<100; ++s) scheduler.update(s*1000U+2,0.5f,true);
  scheduler.update(100003,100,false);
  d=scheduler.update(100004,0.5f,true); assert(!d.groupA && !d.groupB); // no backlog
  d=scheduler.update(100005,NAN,true); assert(!d.groupA && !d.groupB);
  d=scheduler.update(100006,0,true); assert(!d.groupA && !d.groupB);
  scheduler.reset();
  uint32_t now=UINT32_MAX-500U;
  unsigned units=0;
  for (unsigned n=0; n<200; ++n) {
    d=scheduler.update(now,25,true); units+=d.groupA+d.groupB; now+=1000U;
  }
  assert(units==100);
  scheduler.reset();
  scheduler.update(0,1,true);
  d=scheduler.update(3600000,1,true); assert(!d.groupA && !d.groupB); // stalled time earns no heat
  // Variable demand: accumulated energy remains within ONE group-quantum even over 1M slots.
  scheduler.reset(); double requested=0; units=0;
  for (unsigned n=0; n<1000000; ++n) {
    const float power=(n%997)*0.1f;
    d=scheduler.update(n*1000U,power,true);
    if (power==0) requested=units; // explicit zero deliberately discards fractional credit
    else requested+=2.0*power/100.0;
    units+=d.groupA+d.groupB;
    assert(std::fabs(requested-units)<1.00001);
  }
  uint8_t remainder=0;
  assert(HeaterBurstScheduler::fullPowerEquivalentMs(1000,1,true,false,remainder)==1000);
  assert(HeaterBurstScheduler::fullPowerEquivalentMs(1000,2,true,false,remainder)==500);
  assert(HeaterBurstScheduler::fullPowerEquivalentMs(1000,2,true,true,remainder)==1000);
  unsigned equivalent=0;
  for(unsigned n=0;n<1000;++n) equivalent+=HeaterBurstScheduler::fullPowerEquivalentMs(1,2,true,false,remainder);
  assert(equivalent==500 && remainder==0);
  MachineConfig cfg,result;
  RelayAutoTune tune;
  tune.configure(37.5f); tune.start(1000,36);
  tune.update(1000,36,cfg,result);
  scheduler.reset(); units=0;
  for (unsigned n=0;n<600;++n) {
    d=scheduler.update(1000U+n*1000U,tune.power(),true); units+=d.groupA+d.groupB;
  }
  assert(units==360); // autotune 30% total, not a direct GPIO bypass
  tune.abort(); d=scheduler.update(601001,tune.power(),true);
  assert(!d.groupA && !d.groupB);
}
static void burstTimingJitter() {
  HeaterBurstScheduler late(1);
  assert(!late.update(0,50,true).groupA);
  assert(late.update(1999,50,true).groupA);
  assert(late.update(2000,50,true).groupA); // must not emit a 1 ms catch-up pulse
  assert(!late.update(2999,50,true).groupA);
  for(unsigned groups:{1U,2U}) for(float power:{0.5f,1.f,2.f,3.f,5.f,10.f,25.f,50.f,75.f,100.f}) {
    HeaterBurstScheduler scheduler(groups);
    uint32_t now=0,lastChange=0;
    uint64_t a=0,b=0;
    auto previous=scheduler.update(0,power,true);
    while(now<3600000) {
      const uint32_t dt=std::min<uint32_t>(3600000-now,25+(now%51));
      a+=static_cast<uint32_t>(previous.groupA)*dt;
      b+=static_cast<uint32_t>(previous.groupB)*dt;
      now+=dt;
      auto d=scheduler.update(now,power,true);
      if(d.groupA!=previous.groupA || d.groupB!=previous.groupB) {
        assert(now-lastChange>=1000);lastChange=now;
      }
      if(groups==2 && power<=50)assert(!(d.groupA&&d.groupB));
      previous=d;
    }
    const double requested=3600000.0*groups*power/100.0;
    assert(std::fabs(requested-(a+b))<=1150); // one quantum plus <=2 late-poll intervals
    if(groups==2)assert(std::fabs(static_cast<double>(a)-b)<=1075); // actual runtime, not only slot count
  }
}
static void defaultPidPreservesLegacy() {
  MachineConfig cfg;cfg.kp=18;cfg.ki=0.8f;cfg.kd=45;
  ThermalController current;Old::ThermalController legacy;
  for(uint32_t n=0;n<200000;++n) {
    float pv=35+3*std::sin(n*0.007f),sp=n%20000<10000 ? 37.5f:32.0f;
    bool enabled=n%701!=0;
    uint32_t now=UINT32_MAX-1000000U+n*2000U;
    if(n%15001==0) {
      cfg.maxHeaterPower=n%2?33:100;
      current.applyConfigBumpless(now,sp,pv,cfg);legacy.applyConfigBumpless(now,sp,pv,cfg);
    }
    const float a=current.updateOnNewSample(now,sp,pv,cfg,enabled);
    const float b=legacy.updateOnNewSample(now,sp,pv,cfg,enabled);
    assert(std::fabs(a-b)<0.00001f);
  }
  Old::LegacySsrWindow window;
  for(float power : {0.5f,1.f,2.f})
    for(uint32_t ms=1000;ms<120001;ms+=50) assert(!window.ssrWindowOn(ms,power,10));
}
int main() {
  defaultPidPreservesLegacy(); sensorFormats(); pidWeightsAndPermits(); burstEnergy(); burstTimingJitter();
  std::puts("Thermal V2: raw formats/lock/fault, 2-DOF/bumpless/AW, low duty/balance/safety/autotune/rollover/1M slots PASS");
}
