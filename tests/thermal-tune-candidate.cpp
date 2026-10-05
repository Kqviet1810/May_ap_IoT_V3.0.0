#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_tune_candidate.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>

static bool near(float a,float b,float rel=1e-5f){return std::fabs(a-b)<=rel*fmaxf(1.0f,std::fabs(b));}
static ThermalPlantModel fopdt(float K,float tau,float theta){ThermalPlantModel m;m.valid=true;m.reason=ThermalPlantIdReason::Valid;m.mode=ThermalPlantModelMode::FOPDT;m.processGain=K;m.tauSec=tau;m.thetaSec=theta;m.kPrime=K/tau;return m;}
static ThermalPlantModel slow(float kp,float theta){ThermalPlantModel m;m.valid=true;m.reason=ThermalPlantIdReason::Valid;m.mode=ThermalPlantModelMode::SlowSlope;m.thetaSec=theta;m.kPrime=kp;return m;}

int main(){
  {
    const auto m=fopdt(80.0f,1200.0f,30.0f);const float tauC=300.0f;
    const float expectedKc=(1200.0f/80.0f)/(tauC+30.0f);
    const float expectedTi=fminf(1200.0f,4.0f*(tauC+30.0f));
    const float expectedKp=100.0f*expectedKc,expectedKi=expectedKp/expectedTi;
    const auto c=generateThermalSimcPiCandidate(m,tauC);
    std::printf("SIMC FOPDT expected Kc=%.9f Kp=%.9f Ki=%.9f Ti=%.3f | actual Kc=%.9f Kp=%.9f Ki=%.9f Ti=%.3f Kd=%.1f\n",
      expectedKc,expectedKp,expectedKi,expectedTi,c.kcFractionPerC,c.Kp,c.Ki,c.Ti,c.Kd);
    assert(c.valid&&near(c.kcFractionPerC,expectedKc)&&near(c.Kp,expectedKp)&&near(c.Ki,expectedKi)&&near(c.Ti,expectedTi));
    assert(near(c.Kp,100.0f*c.kcFractionPerC)); // mandatory fraction -> runtime percent x100
    assert(c.Kd==0.0f);
  }
  {
    const auto m=slow(0.0125f,45.0f);const float tauC=360.0f;
    const float expectedKc=1.0f/(0.0125f*(tauC+45.0f));
    const float expectedTi=4.0f*(tauC+45.0f);
    const float expectedKp=100.0f*expectedKc,expectedKi=expectedKp/expectedTi;
    const auto c=generateThermalSimcPiCandidate(m,tauC);
    std::printf("SIMC SLOW  expected Kc=%.9f Kp=%.9f Ki=%.9f Ti=%.3f | actual Kc=%.9f Kp=%.9f Ki=%.9f Ti=%.3f Kd=%.1f\n",
      expectedKc,expectedKp,expectedKi,expectedTi,c.kcFractionPerC,c.Kp,c.Ki,c.Ti,c.Kd);
    assert(c.valid&&near(c.kcFractionPerC,expectedKc)&&near(c.Kp,expectedKp)&&near(c.Ki,expectedKi)&&near(c.Ti,expectedTi));
    assert(near(c.Kp,100.0f*c.kcFractionPerC));assert(c.Kd==0.0f);
  }
  for(const auto m:{fopdt(100.0f,1500.0f,30.0f),slow(0.01f,30.0f)}){
    const auto a=generateThermalSimcPiCandidate(m,60.0f),b=generateThermalSimcPiCandidate(m,300.0f),c=generateThermalSimcPiCandidate(m,900.0f);
    assert(a.valid&&b.valid&&c.valid);assert(a.Kp>b.Kp&&b.Kp>c.Kp);assert(std::isfinite(a.Kp)&&std::isfinite(b.Kp)&&std::isfinite(c.Kp));
    assert(a.Kd==0.0f&&b.Kd==0.0f&&c.Kd==0.0f);
  }
  {
    auto m=fopdt(80.0f,1200.0f,100.0f);assert(!generateThermalSimcPiCandidate(m,99.0f).valid);
    assert(generateThermalSimcPiCandidate(m,99.0f).reason==ThermalTuneCandidateReason::TauCBelowTheta);
  }
  const float tauC=600.0f;
  struct Example{const char *name;ThermalPlantModel model;};
  const Example examples[]={
    {"light",fopdt(16000.0f/120.0f,180000.0f/120.0f,30.0f)},
    {"medium",fopdt(16000.0f/180.0f,600000.0f/180.0f,30.0f)},
    {"heavy",slow((16000.0f/300.0f)/(1600000.0f/300.0f),60.0f)}
  };
  for(const auto &e:examples){const auto c=generateThermalSimcPiCandidate(e.model,tauC);assert(c.valid);
    std::printf("EXAMPLE tauC=600 %s mode=%s Kp=%.6f Ki=%.9f Kd=%.1f Ti=%.1f\n",e.name,thermalPlantModelModeName(e.model.mode),c.Kp,c.Ki,c.Kd,c.Ti);}
  std::puts("Thermal SIMC PI candidate: analytic formulas, x100 scale, tauC monotonicity, Kd=0 PASS");
}
