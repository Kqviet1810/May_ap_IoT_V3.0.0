// Execute the actual pinned IDF async-connect state machine with POSIX select.
// TLS crypto/creation are HAL boundaries; TCP readiness and fd_set mutation are real.
#include <cassert>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>

using esp_err_t = int;
enum esp_tls_conn_state_t { ESP_TLS_INIT, ESP_TLS_CONNECTING, ESP_TLS_HANDSHAKE, ESP_TLS_FAIL, ESP_TLS_DONE };
constexpr int ESP_OK=0;
struct esp_tls_cfg_t { bool is_plain_tcp=false,non_block=true;int timeout_ms=1; };
struct esp_tls_t {
  esp_tls_conn_state_t conn_state=ESP_TLS_INIT;int sockfd=-1,error_handle=0;
  bool is_tls=false;fd_set rset{},wset{};int (*read)()=nullptr,(*write)()=nullptr;
};
static int chosenFd,sslCreates=0,handshakes=0;
static bool tlsPending=false;
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_INT_EVENT_TRACKER_CAPTURE(...) ((void)0)
#define _esp_tls_net_init(tls) ((void)0)
static int tcp_read(){return 0;}
static int tcp_write(){return 0;}
#define _esp_tls_read tcp_read
#define _esp_tls_write tcp_write
static esp_err_t tcp_connect(const char*,int,int,const esp_tls_cfg_t*,int,int *fd){*fd=chosenFd;return ESP_OK;}
static int create_ssl_handle(const char*,int,const esp_tls_cfg_t*,esp_tls_t*){++sslCreates;return ESP_OK;}
static int esp_tls_handshake(esp_tls_t *tls,const esp_tls_cfg_t*){
  ++handshakes;if(tlsPending)return 0;tls->conn_state=ESP_TLS_DONE;return 1;
}
static void ms_to_timeval(int ms,timeval *tv){tv->tv_sec=ms/1000;tv->tv_usec=(ms%1000)*1000;}
#include "fixtures/esp-idf/async-connect.inc"
static int esp_tls_conn_new_async(const char *host,int length,int port,const esp_tls_cfg_t *cfg,esp_tls_t *tls){
  return esp_tls_low_level_conn(host,length,port,cfg,tls);
}
#define ESP_IDF_VERSION_VAL(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5,5,5)
#include "actual-esp-tls-poll.inc"

int main(){
  for(int round=0;round<200;round++){
    int pair[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);chosenFd=pair[0];
    assert(fcntl(pair[0],F_SETFL,O_NONBLOCK)==0);assert(fcntl(pair[1],F_SETFL,O_NONBLOCK)==0);
    char bytes[4096]={};while(send(pair[0],bytes,sizeof(bytes),0)>0){}
    assert(errno==EAGAIN || errno==EWOULDBLOCK);
    esp_tls_cfg_t cfg;esp_tls_t tls;sslCreates=handshakes=0;
    // No TCP readiness on first poll: IDF clears the caller's fd_sets.
    assert(esp_tls_conn_new_async("1.2.3.4",7,443,&cfg,&tls)==0);
    assert(tls.conn_state==ESP_TLS_CONNECTING&&!FD_ISSET(chosenFd,&tls.rset)&&!FD_ISSET(chosenFd,&tls.wset));
    while(recv(pair[1],bytes,sizeof(bytes),0)>0){}
    fd_set ready;FD_ZERO(&ready);FD_SET(chosenFd,&ready);timeval immediate{};
    assert(select(chosenFd+1,nullptr,&ready,nullptr,&immediate)==1);
    // Even with real socket readiness, the unmodified SDK keeps timing out.
    for(int poll=0;poll<3;poll++)assert(esp_tls_conn_new_async("1.2.3.4",7,443,&cfg,&tls)==0);
    assert(tls.conn_state==ESP_TLS_CONNECTING&&sslCreates==0&&handshakes==0);
    // The production repair reaches TLS without extending either timeout.
    tlsPending=true;
    assert(MayapEspTlsPoll::connectAsync("1.2.3.4",7,443,&cfg,&tls)==0);
    assert(tls.conn_state==ESP_TLS_HANDSHAKE&&sslCreates==1&&handshakes==1);
    for(int poll=0;poll<3;poll++)assert(MayapEspTlsPoll::connectAsync("1.2.3.4",7,443,&cfg,&tls)==0);
    assert(sslCreates==1);tlsPending=false;
    assert(MayapEspTlsPoll::connectAsync("1.2.3.4",7,443,&cfg,&tls)==1);
    assert(tls.conn_state==ESP_TLS_DONE&&sslCreates==1&&cfg.timeout_ms==1);
    close(pair[0]);close(pair[1]);
  }
  puts("Actual IDF 5.5.5 + real select: reproduced empty fd_sets, repaired TCP readiness, unchanged TLS polling and 200 cleanup cycles PASS");
}
