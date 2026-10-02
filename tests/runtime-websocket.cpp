// Actual RFC6455 parser and ESP transport with fault-injected DNS/TLS/TCP.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>
#include <algorithm>
#include <new>
#include <cstdarg>
#include <cerrno>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include "../MAYAP_INDUSTRIAL_v1_0_0/websocket_codec.h"
using std::min;
static uint32_t clockMs=100;
uint32_t millis(){return clockMs;}
uint32_t esp_random(){return 0x12345678;}
void esp_fill_random(void *p,size_t n){memset(p,17,n);}
static int admissions=0,tlsCount=0,tlsResult=1,dnsResult=0,writeLimit=7,writeCalls=0,readCalls=0;
static bool admit=true,writeBlocked=false,eof=false;
class MayapTlsOperation{public:MayapTlsOperation(){++admissions;}~MayapTlsOperation(){--admissions;}explicit operator bool()const{return admit;}};
enum esp_tls_conn_state_t { ESP_TLS_INIT, ESP_TLS_CONNECTING, ESP_TLS_HANDSHAKE, ESP_TLS_FAIL, ESP_TLS_DONE };
constexpr int ESP_OK=0;
struct esp_tls_t{esp_tls_conn_state_t conn_state=ESP_TLS_INIT;int sockfd=42;fd_set rset{},wset{};};
static esp_tls_conn_state_t tlsMockState=ESP_TLS_HANDSHAKE;
static int stateCalls=0,fdCalls=0,peerCalls=0,fdResult=0,peerResult=0;
static int socketOptionCalls=0,socketOptionResult=0,socketFd=42;
static bool realSocketOption=false;
int esp_tls_get_conn_state(esp_tls_t *tls,esp_tls_conn_state_t *state){++stateCalls;*state=tls->conn_state;return ESP_OK;}
int esp_tls_get_conn_sockfd(esp_tls_t *tls,int *fd){++fdCalls;*fd=tls->sockfd;return fdResult;}
int socketOption(int fd,int level,int option,const void *value,socklen_t size){
 ++socketOptionCalls;assert(fd==socketFd&&level==IPPROTO_TCP&&option==TCP_NODELAY&&size==sizeof(int)&&*static_cast<const int*>(value)==1);
 if(realSocketOption)return ::setsockopt(fd,level,option,value,size);
 if(socketOptionResult<0)errno=ENOPROTOOPT;
 return socketOptionResult;
}
int getpeername(int fd,sockaddr*,socklen_t*) noexcept {assert(fd==42);++peerCalls;if(peerResult<0)errno=ENOTCONN;return peerResult;}
#ifndef MAYAP_DIAGNOSTIC_SERIAL
#define MAYAP_DIAGNOSTIC_SERIAL 1
#endif
#define ESP_ARDUINO_VERSION_STR "host-core"
const char *esp_get_idf_version(){return "host-idf";}
static std::vector<std::string> diagnostics;
void mayapSerialPrintf(bool,const char *format,...){char line[224];va_list args;va_start(args,format);int n=vsnprintf(line,sizeof(line),format,args);va_end(args);assert(n>0&&n<int(sizeof(line)));diagnostics.emplace_back(line);}
struct esp_tls_cfg_t{const unsigned char *cacert_buf=nullptr;size_t cacert_bytes=0;const char *common_name=nullptr;bool non_block=false;int timeout_ms=0;};
constexpr int ESP_TLS_ERR_SSL_WANT_READ=-10,ESP_TLS_ERR_SSL_WANT_WRITE=-11;
static std::string input,output;static size_t readOffset=0;
static bool tcpPolling=false;static unsigned tcpPolls=0,tcpReadyAfter=3;
esp_tls_t *esp_tls_init(){++tlsCount;auto *tls=new esp_tls_t;tls->sockfd=socketFd;return tls;}
void esp_tls_conn_destroy(esp_tls_t *p){--tlsCount;delete p;}
int esp_tls_conn_new_async(const char *ip,int,int port,const esp_tls_cfg_t *cfg,esp_tls_t *tls){
 assert(!strcmp(ip,"1.2.3.4"));assert(port==443&&cfg->non_block&&cfg->timeout_ms==1);assert(!strcmp(cfg->common_name,"hub.test"));assert(cfg->cacert_bytes==3);
 if(tcpPolling){
  if(tls->conn_state==ESP_TLS_INIT){tls->conn_state=ESP_TLS_CONNECTING;FD_ZERO(&tls->rset);FD_SET(tls->sockfd,&tls->rset);tls->wset=tls->rset;}
  if(tls->conn_state==ESP_TLS_CONNECTING){
   ++tcpPolls;
   bool ready=tcpPolls>=tcpReadyAfter&&(FD_ISSET(tls->sockfd,&tls->rset)||FD_ISSET(tls->sockfd,&tls->wset));
   // Exact SDK/lwIP timeout behavior: select clears both descriptor sets.
   FD_ZERO(&tls->rset);FD_ZERO(&tls->wset);
   if(!ready)return 0;
   tls->conn_state=ESP_TLS_HANDSHAKE;return 0;
  }
 }
 tls->conn_state=tlsResult>0?ESP_TLS_DONE:tlsResult<0?ESP_TLS_FAIL:tlsMockState;return tlsResult;
}
int esp_tls_conn_write(esp_tls_t*,const void *p,size_t n){++writeCalls;if(writeBlocked)return ESP_TLS_ERR_SSL_WANT_WRITE;const size_t len=min(n,static_cast<size_t>(writeLimit));output.append(static_cast<const char*>(p),len);return len;}
int esp_tls_conn_read(esp_tls_t*,void *p,size_t n){++readCalls;if(readOffset==input.size())return eof?0:ESP_TLS_ERR_SSL_WANT_READ;const size_t len=min(n,input.size()-readOffset);memcpy(p,input.data()+readOffset,len);readOffset+=len;return len;}
int mbedtls_base64_encode(unsigned char *out,size_t,size_t *n,const unsigned char*,size_t len){const char *v=len==16?"ABCD":"EXPECTED_ACCEPT";strcpy(reinterpret_cast<char*>(out),v);*n=strlen(v);return 0;}
int mbedtls_sha1(const uint8_t*,size_t,uint8_t *out){memset(out,1,20);return 0;}
struct ip_addr_t{uint8_t b[4]={1,2,3,4};};
using err_t=int;constexpr int ERR_OK=0,ERR_INPROGRESS=1,LWIP_DNS_ADDRTYPE_IPV4=0;
#define LOCK_TCPIP_CORE() ((void)0)
#define UNLOCK_TCPIP_CORE() ((void)0)
#define IP_IS_V4(a) true
#define ip_2_ip4(a) (a)
#define ip4_addr1(a) ((a)->b[0])
#define ip4_addr2(a) ((a)->b[1])
#define ip4_addr3(a) ((a)->b[2])
#define ip4_addr4(a) ((a)->b[3])
static void(*dnsCallback)(const char*,const ip_addr_t*,void*)=nullptr;static void *dnsArg=nullptr;
err_t dns_gethostbyname_addrtype(const char*,ip_addr_t *a,void(*callback)(const char*,const ip_addr_t*,void*),void *arg,int){dnsCallback=callback;dnsArg=arg;*a=ip_addr_t{};return dnsResult;}
#define ESP_IDF_VERSION_VAL(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5,5,5)
#include "actual-esp-tls-poll.inc"
#define setsockopt socketOption
#include "actual-websocket-transport.inc"
#undef setsockopt
static std::vector<std::string> received;
void receive(const uint8_t *p,size_t n){received.emplace_back(reinterpret_cast<const char*>(p),n);}
std::vector<uint8_t> frame(uint8_t op,const std::string &data,bool fin=true){std::vector<uint8_t> v{static_cast<uint8_t>((fin?128:0)|op)};if(data.size()<126)v.push_back(data.size());else{v.push_back(126);v.push_back(data.size()>>8);v.push_back(data.size()&255);}v.insert(v.end(),data.begin(),data.end());return v;}
void reset(){input.clear();output.clear();readOffset=0;tlsResult=1;dnsResult=0;writeLimit=7;writeBlocked=eof=false;admit=true;clockMs=100;received.clear();diagnostics.clear();tlsMockState=ESP_TLS_HANDSHAKE;stateCalls=fdCalls=peerCalls=fdResult=peerResult=0;tcpPolling=false;tcpPolls=0;tcpReadyAfter=3;socketOptionCalls=socketOptionResult=0;socketFd=42;realSocketOption=false;}
size_t diagnosticCount(const std::string &text){return std::count_if(diagnostics.begin(),diagnostics.end(),[&](const std::string &line){return line.find(text)!=std::string::npos;});}
void begin(WebSocketTransport &ws){assert(ws.begin("hub.test","MAP-1234567890AB",std::string(64,'a').c_str(),123,"CA",clockMs));}
void open(WebSocketTransport &ws,const std::string &response="HTTP/1.1 101 Switching Protocols\r\nUpgrade: WebSocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: EXPECTED_ACCEPT\r\n\r\n"){
 input=response;ws.setCallback(receive);begin(ws);for(unsigned i=0;i<100&&ws.busy()&&!ws.connected();++i){clockMs+=10;ws.loop(clockMs);}assert(ws.connected());assert(admissions==0);assert(output.find("Authorization: Bearer "+std::string(64,'a'))!=std::string::npos);
}
int main(){
 using namespace MayapWebSocket;
 // Configure the actual TCP socket option once, only after TLS succeeds.
 {reset();WebSocketTransport ws;tlsResult=0;begin(ws);ws.loop(clockMs);
  assert(fdCalls==0&&socketOptionCalls==0);tlsResult=1;
  input="HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: EXPECTED_ACCEPT\r\n\r\n";
  for(int i=0;i<100&&!ws.connected();i++)ws.loop(clockMs+=10);
  assert(ws.connected()&&fdCalls==1&&socketOptionCalls==1);
  for(int i=0;i<100;i++)ws.loop(clockMs+1);
  assert(fdCalls==1&&socketOptionCalls==1);ws.disconnect();}
 // Read back TCP_NODELAY on a real OS socket; this is not just a HAL flag.
 {reset();socketFd=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);assert(socketFd>=0);realSocketOption=true;
  int option=-1;socklen_t size=sizeof(option);
  assert(::getsockopt(socketFd,IPPROTO_TCP,TCP_NODELAY,&option,&size)==0&&option==0);
  WebSocketTransport ws;open(ws);size=sizeof(option);
  assert(::getsockopt(socketFd,IPPROTO_TCP,TCP_NODELAY,&option,&size)==0&&option==1&&socketOptionCalls==1);
  ws.disconnect();assert(::close(socketFd)==0);}
 // A missing fd or unsupported performance option never breaks connectivity.
 for(int mode=0;mode<2;mode++){reset();fdResult=mode==0?-7:0;socketOptionResult=mode==1?-1:0;
  WebSocketTransport ws;open(ws);assert(fdCalls==1&&socketOptionCalls==(mode==0?0:1));
#if MAYAP_DIAGNOSTIC_SERIAL
  assert(diagnosticCount("tcp_nodelay=0")==1);
#endif
  ws.disconnect();}
 // The unmodified SDK stalls after its first select timeout, even when TCP is ready.
 {reset();tcpPolling=true;esp_tls_t tls;esp_tls_cfg_t cfg;cfg.non_block=true;cfg.timeout_ms=1;cfg.common_name="hub.test";cfg.cacert_bytes=3;
  for(int i=0;i<100;i++)assert(esp_tls_conn_new_async("1.2.3.4",7,443,&cfg,&tls)==0);
  assert(tls.conn_state==ESP_TLS_CONNECTING&&tcpPolls==100);}
 // Production repair re-arms after any number of timeouts and reaches Open.
 for(unsigned wait:{3U,100U,600U}){reset();tcpPolling=true;tcpReadyAfter=wait;writeLimit=10000;WebSocketTransport ws;
  input="HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: EXPECTED_ACCEPT\r\n\r\n";
  begin(ws);for(unsigned i=0;i<wait+2&&!ws.connected();i++)ws.loop(clockMs+=20);
  assert(ws.connected()&&tcpPolls==wait&&admissions==0);ws.disconnect();assert(tlsCount==0);}
 // Repair never overflows fd_set and never changes TLS handshake polling.
 {reset();esp_tls_t tls;tls.conn_state=ESP_TLS_CONNECTING;tls.sockfd=FD_SETSIZE;
  esp_tls_cfg_t cfg;cfg.non_block=true;assert(MayapEspTlsPoll::connectAsync("1.2.3.4",7,443,&cfg,&tls)==-1);assert(errno==EBADF&&tls.conn_state==ESP_TLS_FAIL);}
#if MAYAP_DIAGNOSTIC_SERIAL
 // Versions once, transitions once, and no handshake/header secrets in logs.
 {reset();WebSocketTransport::logVersionsOnce();WebSocketTransport::logVersionsOnce();assert(diagnosticCount("arduino=host-core idf=host-idf")==1);}
 {reset();WebSocketTransport ws;open(ws);
  for(const char *phase:{"DNS","TLS","UPGRADE","OPEN"})assert(diagnosticCount(std::string("phase=")+phase+"\n")==1);
  assert(diagnosticCount("http_status=101 websocket_valid=1")==1);
  auto before=diagnostics.size();for(int i=0;i<100;i++)ws.loop(clockMs+1);assert(diagnostics.size()==before);
  for(const auto &line:diagnostics){assert(line.find(std::string(64,'a'))==std::string::npos);assert(line.find("Authorization")==std::string::npos);assert(line.find("ABCD")==std::string::npos);assert(line.find("EXPECTED_ACCEPT")==std::string::npos);}
  ws.disconnect();
 }
 // TCP pending, both peer outcomes, and getter failure: exactly one probe at timeout.
 for(int mode=0;mode<3;mode++){reset();WebSocketTransport ws;tlsResult=0;tlsMockState=ESP_TLS_CONNECTING;
  peerResult=mode==1?-1:0;fdResult=mode==2?-7:0;begin(ws);
  for(int i=0;i<100;i++)ws.loop(clockMs+1);
  assert(diagnosticCount("tls_conn_state=0\n")==1&&diagnosticCount("tls_conn_state=1\n")==1);
  assert(fdCalls==0&&peerCalls==0);ws.loop(clockMs+15001);assert(ws.state()==-3&&!ws.busy());
  assert(diagnosticCount("timeout elapsed_ms=15001 phase=TLS dnsReady=1 tls_conn_state=1 upgradeSent=0 upgradeUsed=0")==1);
  assert(fdCalls==1&&peerCalls==(mode==2?0:1));
  assert(diagnosticCount(mode==0?"peer_established=1 fd_result=0 result=0 errno=0":mode==1?"peer_established=0 fd_result=0 result=-1 errno="+std::to_string(ENOTCONN):"peer_established=0 fd_result=-7 result=-1 errno=0")==1);
  ws.loop(clockMs+16000);assert(fdCalls==1&&peerCalls==(mode==2?0:1));
 }
 // TLS WANT_READ polls and even abnormal state oscillation cannot flood Serial.
 {reset();WebSocketTransport ws;tlsResult=0;begin(ws);ws.loop(clockMs);
  auto count=diagnostics.size();for(int i=0;i<100;i++)ws.loop(clockMs+1);assert(diagnostics.size()==count);
  for(int i=0;i<100;i++){tlsMockState=i%2?ESP_TLS_CONNECTING:ESP_TLS_HANDSHAKE;ws.loop(clockMs+1);}
  assert(diagnosticCount("[WS-CONNECT] tls_conn_state=")<=5);ws.disconnect();}
 {reset();WebSocketTransport ws;tlsResult=0;begin(ws);ws.loop(clockMs);ws.loop(clockMs+15001);
  assert(diagnosticCount("phase=TLS dnsReady=1 tls_conn_state=2")==1&&peerCalls==0&&fdCalls==0);}
 {reset();WebSocketTransport ws;dnsResult=ERR_INPROGRESS;begin(ws);ws.loop(clockMs+15001);
  assert(diagnosticCount("phase=DNS dnsReady=0 tls_conn_state=-1")==1&&stateCalls==0&&peerCalls==0);
  dnsCallback("hub.test",nullptr,dnsArg);}
 // Rejected and incomplete HTTP responses disclose only numeric status/validation.
 {reset();WebSocketTransport ws;input="HTTP/1.1 401 Unauthorized\r\nAuthorization: PRIVATE_RESPONSE_SECRET\r\n\r\n";begin(ws);
  for(int i=0;i<100&&ws.busy();i++)ws.loop(clockMs+=10);
  assert(ws.state()==-9&&diagnosticCount("http_status=401 websocket_valid=0")==1);
  for(const auto &line:diagnostics)assert(line.find("PRIVATE_RESPONSE_SECRET")==std::string::npos);}
 {reset();WebSocketTransport ws;writeLimit=10000;input="HTTP/1.1 403 Forbidden\r\n";begin(ws);ws.loop(clockMs);
  assert(diagnosticCount("http_status=")==0);ws.loop(clockMs+15001);
  assert(ws.state()==-3&&diagnosticCount("http_status=403 websocket_valid=-1")==1&&peerCalls==0);}
#endif
 // Random TCP slicing for text sizes through the hard bound, including UTF8
 // split across continuation frames and interleaved protocol Ping/Pong.
 for(unsigned n=0;n<=2048;n+=17){Reader r;const std::string body(n,'x');auto bytes=frame(1,body);unsigned calls=0;
  for(size_t i=0;i<bytes.size();){size_t take=min(static_cast<size_t>(1+(i*13)%37),bytes.size()-i);assert(r.push(bytes.data()+i,take,[&](uint8_t op,const uint8_t *p,size_t size){assert(op==1&&std::string(reinterpret_cast<const char*>(p),size)==body);++calls;return true;}));i+=take;}assert(calls==1);
 }
 {Reader r;auto a=frame(1,"\xe1\xbb",false),ping=frame(9,"abc"),b=frame(0,"\x87");unsigned text=0,control=0;
  const auto h=[&](uint8_t op,const uint8_t *p,size_t n){if(op==9)++control;else{assert(std::string(reinterpret_cast<const char*>(p),n)=="\xe1\xbb\x87");++text;}return true;};assert(r.push(a.data(),a.size(),h));assert(r.push(ping.data(),ping.size(),h));assert(r.push(b.data(),b.size(),h));assert(text==1&&control==1);
 }
 for(const auto &bad:std::vector<std::vector<uint8_t>>{{0xc1,0},{0x82,0},{0x80,0},{0x09,0},{0x81,0x80},{0x81,126,0,1},{0x81,126,8,1},{0x81,127,0,0,0,0,0,0,0,126},{0x88,1,0},{0x81,2,0xc0,0x80},{0x81,3,0xed,0xa0,0x80}}){Reader r;assert(!r.push(bad.data(),bad.size(),[](uint8_t,const uint8_t*,size_t){return true;}));}
 {Reader r;auto a=frame(1,std::string(2048,'x'),false),b=frame(0,"x");assert(r.push(a.data(),a.size(),[](uint8_t,const uint8_t*,size_t){return true;}));assert(!r.push(b.data(),b.size(),[](uint8_t,const uint8_t*,size_t){return true;}));}
 for(size_t n: {size_t(0),size_t(125),size_t(126),size_t(2048)}){uint8_t bytes[2056];std::vector<uint8_t> data(n,0x55);const size_t count=clientFrame(bytes,sizeof(bytes),1,data.data(),n,0x12345678);assert(count==n+(n<126?6:8));assert(bytes[1]&128);for(size_t i=0;i<n;++i)assert((bytes[count-n+i]^bytes[count-n-4+i%4])==0x55);}
 // Partial handshake/write, coalesced first frame, nonblocking read and queue.
 {reset();WebSocketTransport ws;auto first=frame(1,"{\"first\":true}");std::string response="HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: EXPECTED_ACCEPT\r\n\r\n";response.append(reinterpret_cast<const char*>(first.data()),first.size());open(ws,response);assert(received.size()==1);writeBlocked=true;
  for(int i=0;i<8;i++){assert(ws.send("snapshot","{}",2));}assert(!ws.send("snapshot","{}",2));ws.loop(clockMs+1);assert(ws.connected());writeBlocked=false;writeLimit=10000;
  int calls=writeCalls;ws.loop(clockMs+2);assert(writeCalls-calls<=4);ws.disconnect();assert(admissions==0&&tlsCount==0);
 }
 // Portal/radio recovery cancels an in-progress handshake and releases admission.
 {reset();WebSocketTransport ws;tlsResult=0;begin(ws);ws.loop(clockMs);assert(ws.busy()&&!ws.connected());ws.disconnect();assert(!ws.busy()&&admissions==0&&tlsCount==0);}
 // TLS handshake, asynchronous DNS, network stalls and wrap-around deadlines.
 {reset();WebSocketTransport ws;tlsResult=0;begin(ws);ws.loop(clockMs);assert(ws.busy()&&!ws.connected()&&admissions==1);ws.loop(clockMs+15001);assert(!ws.busy()&&admissions==0&&tlsCount==0);}
 {reset();WebSocketTransport ws;dnsResult=ERR_INPROGRESS;begin(ws);ws.loop(clockMs+15001);assert(!ws.busy()&&admissions==0);assert(!ws.begin("hub.test","MAP-1234567890AB",std::string(64,'a').c_str(),123,"CA",clockMs));ip_addr_t ip;dnsCallback("hub.test",&ip,dnsArg);dnsResult=0;begin(ws);ws.disconnect();assert(admissions==0);}
 {reset();WebSocketTransport ws;dnsResult=-1;begin(ws);ws.loop(clockMs);assert(ws.state()==-4&&admissions==0);}
 {reset();WebSocketTransport ws;admit=false;assert(!ws.begin("hub.test","MAP-1234567890AB",std::string(64,'a').c_str(),123,"CA",clockMs));assert(admissions==0);}
 {reset();WebSocketTransport ws;open(ws);writeBlocked=true;assert(ws.send("ack","{}",2));ws.loop(clockMs+10001);assert(!ws.busy()&&tlsCount==0);}
 {reset();WebSocketTransport ws;open(ws);ws.loop(clockMs+90001);assert(!ws.busy());}
 {reset();WebSocketTransport ws;clockMs=0xfffffff0;open(ws);eof=true;ws.loop(clockMs+1);assert(!ws.busy());}
 {reset();WebSocketTransport ws;input="HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: WRONG\r\n\r\n";begin(ws);for(int i=0;i<100&&ws.busy();i++)ws.loop(clockMs+=10);assert(!ws.connected()&&ws.state()==-9&&tlsCount==0);
#if MAYAP_DIAGNOSTIC_SERIAL
  assert(diagnosticCount("http_status=101 websocket_valid=0")==1);
#endif
 }
 // 20,000 reconnects stress the actual object's resource cleanup with ASAN.
 for(int i=0;i<20000;i++){reset();WebSocketTransport ws;writeLimit=10000;open(ws);assert(socketOptionCalls==1);ws.disconnect();assert(admissions==0&&tlsCount==0);}
#if !MAYAP_DIAGNOSTIC_SERIAL
 assert(diagnostics.empty()&&stateCalls==0&&fdCalls==1&&peerCalls==0);
#endif
 puts("WebSocket: strict RFC6455, UTF8, fragmentation, async DNS/TLS, real TCP_NODELAY, bounded diagnostics/queues/deadlines and 20,000 reconnects PASS");
}
