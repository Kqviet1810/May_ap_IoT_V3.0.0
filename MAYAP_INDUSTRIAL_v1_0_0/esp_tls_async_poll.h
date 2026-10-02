#pragma once
#include <esp_tls.h>
#include <esp_idf_version.h>
#include <lwip/sockets.h>
#include <errno.h>

// Narrow compatibility repair for the pinned Arduino 3.3.11 / IDF 5.5.5.
// IDF esp_tls.c initializes rset/wset only in INIT, but select() overwrites
// them on EVERY call, including a timeout. Re-arm CONNECTING before polling.
// Use the SDK's own struct declaration, never guessed offsets or a copied ABI.
// Re-audit this repair when upgrading the SDK rather than silently compiling
// against a different private layout/state machine.
#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 5, 5)
#error "Re-audit esp_tls_async_poll.h for the new ESP-IDF version"
#endif
#include <private_include/esp_tls_private.h>

namespace MayapEspTlsPoll {
inline int connectAsync(const char *host, int length, int port, const esp_tls_cfg_t *cfg, esp_tls_t *tls) {
  if (tls && cfg && cfg->non_block && tls->conn_state == ESP_TLS_CONNECTING) {
#ifdef LWIP_SELECT_MAXNFDS
    // lwIP's socket descriptors may start above zero. Its fd_set size is a
    // socket count, not necessarily the maximum descriptor number.
    const int firstFd = LWIP_SOCKET_OFFSET, maxFd = LWIP_SELECT_MAXNFDS;
#else
    const int firstFd = 0, maxFd = FD_SETSIZE;
#endif
    if (tls->sockfd < firstFd || tls->sockfd >= maxFd) {
      errno = EBADF; tls->conn_state = ESP_TLS_FAIL; return -1;
    }
    FD_ZERO(&tls->rset); FD_SET(tls->sockfd, &tls->rset);
    FD_ZERO(&tls->wset); FD_SET(tls->sockfd, &tls->wset);
  }
  return esp_tls_conn_new_async(host, length, port, cfg, tls);
}
}  // namespace MayapEspTlsPoll
