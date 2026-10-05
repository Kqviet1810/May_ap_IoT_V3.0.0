#include <cstdint>
#include <cassert>
#include <cstdio>
#include <cstdarg>
enum class NetworkStateCode : uint8_t { Offline, NotConfigured, Connecting, Connected };
static uint32_t clockMs=0;
uint32_t millis() { return clockMs; }
void mayapSerialPrintf(bool,const char*,...) {}
bool credentialsConfigured() { return true; }
struct { uint32_t localIP() const { return 1234; } } WiFi;
#include "actual-wifi-globals.inc"
#include "actual-wifi-publish.inc"
void sample(uint32_t time,bool raw) {
 clockMs=time; publish(raw?NetworkStateCode::Connected:NetworkStateCode::Connecting,raw,raw?-50:-127);
}
int main() {
 sample(0,true); sample(250,true); sample(500,true); sample(750,true);
 assert(publishedConnected);
 // Short driver glitch must not appear in HMI/Fault/Web snapshot.
 sample(1000,false); assert(publishedConnected);
 sample(1250,false); sample(1500,false); sample(1750,true); assert(publishedConnected);
 // Continuous four-second failure is published once, not on the first poll.
 sample(2000,false);
 for(uint32_t t=2250;t<6000;t+=250) { sample(t,false); assert(publishedConnected); }
 sample(6000,false); assert(!publishedConnected);
 // One positive sample during router reboot cannot flicker UI online.
 sample(6250,true); assert(!publishedConnected);
 sample(6500,false); assert(!publishedConnected);
 sample(6750,true); sample(7000,true); sample(7250,true); sample(7500,true);
 assert(publishedConnected);
 for(unsigned n=0;n<1000;++n) {
  uint32_t t=10000+n*2000; sample(t,false); sample(t+250,false);sample(t+500,true);sample(t+750,true);
  assert(publishedConnected);
 }
 // Offline is a deliberate setting, not a flap: immediate publication.
 clockMs=3000000; publish(NetworkStateCode::Offline,false); assert(!publishedConnected);
 std::puts("Production Wi-Fi flap publication PASS");
}
