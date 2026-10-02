#pragma once
#include <Arduino.h>
#include <esp_tls.h>
#include "esp_tls_async_poll.h"
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include <new>
#include <errno.h>
#include <lwip/sockets.h>
#include <esp_arduino_version.h>
#include <esp_system.h>
#include "websocket_codec.h"
#include "network_io_guard.h"
#include "serial_diagnostics.h"

// Only the existing dedicated realtime owner touches this transport. DNS,
// TCP/TLS and upgrade are incremental; no socket calls occur in control/ISR.
class WebSocketTransport {
 public:
  static void logVersionsOnce() {
#if MAYAP_DIAGNOSTIC_SERIAL
    static bool logged = false;
    if (!logged) {
      logged = true;
      mayapSerialPrintf(true, "[WS-CONNECT] arduino=%s idf=%s\n", ESP_ARDUINO_VERSION_STR, esp_get_idf_version());
    }
#endif
  }
  using Callback = void (*)(const uint8_t *, size_t);
  void setCallback(Callback callback) { callback_ = callback; }
  bool connected() const { return phase_ == Phase::Open; }
  bool busy() const { return phase_ != Phase::Closed; }
  int state() const { return error_; }
  void disconnect() {
    if (tls_) { esp_tls_conn_destroy(tls_); tls_ = nullptr; }
    delete admission_; admission_ = nullptr;
    phase_ = Phase::Closed; txHead_ = txTail_ = txCount_ = 0; txOffset_ = 0;
    upgradeUsed_ = upgradeSent_ = 0; reader_.reset(); memset(upgrade_, 0, sizeof(upgrade_));
  }
  bool begin(const char *host, const char *id, const char *key, uint32_t boot, const char *ca, uint32_t now) {
    if (busy() || !host || !host[0] || strlen(host) >= sizeof(host_) ||
        !id || strlen(id) != 16 || !key || strlen(key) != 64 || !ca || !ca[0]) return false;
    // A DNS callback owns this static object's DNS mailbox until completion.
    if (__atomic_load_n(&dnsPending_, __ATOMIC_ACQUIRE)) return false;
    admission_ = new(std::nothrow) MayapTlsOperation;
    if (!admission_ || !*admission_) { delete admission_; admission_ = nullptr; return false; }
    snprintf(host_, sizeof(host_), "%s", host);
    uint8_t nonce[16]; esp_fill_random(nonce, sizeof(nonce)); size_t written = 0;
    unsigned char websocketKey[25] = {};
    mbedtls_base64_encode(websocketKey, sizeof(websocketKey), &written, nonce, sizeof(nonce));
    char acceptInput[64]; snprintf(acceptInput, sizeof(acceptInput), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", websocketKey);
    uint8_t digest[20]; mbedtls_sha1(reinterpret_cast<const uint8_t *>(acceptInput), strlen(acceptInput), digest);
    mbedtls_base64_encode(reinterpret_cast<unsigned char *>(accept_), sizeof(accept_), &written, digest, sizeof(digest));
    const int length = snprintf(upgrade_, sizeof(upgrade_),
        "GET /realtime/device/%s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nAuthorization: Bearer %s\r\nX-Mayap-Boot: %lu\r\n\r\n",
        id, host_, websocketKey, key, static_cast<unsigned long>(boot));
    if (length < 0 || static_cast<size_t>(length) >= sizeof(upgrade_)) { fail(-2); return false; }
    upgradeLength_ = length; upgradeSent_ = upgradeUsed_ = 0;
    cfg_ = {}; cfg_.cacert_buf = reinterpret_cast<const unsigned char *>(ca); cfg_.cacert_bytes = strlen(ca) + 1;
    cfg_.common_name = host_; cfg_.non_block = true;
    // esp-tls async uses timeout_ms for select() on EACH poll. Keep it short;
    // the outer state-machine deadline bounds the entire handshake instead.
    cfg_.timeout_ms = 1;
    started_ = lastRx_ = lastTx_ = now; error_ = 0; phase_ = Phase::Dns;
#if MAYAP_DIAGNOSTIC_SERIAL
    lastTlsState_ = -1; tlsStateLogs_ = 0; upgradeResultLogged_ = false;
#endif
    __atomic_store_n(&dnsPending_, 1U, __ATOMIC_RELEASE); __atomic_store_n(&dnsReady_, 0U, __ATOMIC_RELEASE);
    logPhase("DNS");
    ip_addr_t address;
    LOCK_TCPIP_CORE();
    const err_t result = dns_gethostbyname_addrtype(host_, &address, dnsDone, this, LWIP_DNS_ADDRTYPE_IPV4);
    UNLOCK_TCPIP_CORE();
    if (result == ERR_OK) dnsDone(host_, &address, this);
    else if (result != ERR_INPROGRESS) dnsDone(host_, nullptr, this);
    return true;
  }
  bool send(const char *channel, const char *payload, size_t length) {
    if (!connected() || !channel || strlen(channel) > 24 || txCount_ == QUEUE_CAP) return false;
    char text[MayapWebSocket::FRAME_CAP];
    const int prefix = snprintf(text, sizeof(text), "{\"v\":1,\"channel\":\"%s\",\"payload\":", channel);
    if (prefix < 0 || static_cast<size_t>(prefix) + length + 1 > sizeof(text)) return false;
    memcpy(text + prefix, payload, length); text[prefix + length] = '}';
    return enqueue(1, reinterpret_cast<const uint8_t *>(text), prefix + length + 1);
  }
  void loop(uint32_t now) {
    if (!busy()) return;
    if (phase_ != Phase::Open && static_cast<uint32_t>(now - started_) > 15000U) {
      logTimeout(now); fail(-3); return;
    }
    if (phase_ == Phase::Dns) {
      const uint8_t ready = __atomic_load_n(&dnsReady_, __ATOMIC_ACQUIRE);
      if (!ready) return;
      if (ready == 2) { fail(-4); return; }
      snprintf(ip_, sizeof(ip_), "%u.%u.%u.%u", dnsAddress_[0], dnsAddress_[1], dnsAddress_[2], dnsAddress_[3]);
      tls_ = esp_tls_init(); if (!tls_) { fail(-5); return; } phase_ = Phase::Tls;
      logPhase("TLS"); logTlsState();
    }
    if (phase_ == Phase::Tls) {
      const int result = MayapEspTlsPoll::connectAsync(ip_, strlen(ip_), 443, &cfg_, tls_);
      logTlsState();
      if (result < 0) { fail(-6); return; } if (!result) return;
      phase_ = Phase::Upgrade;
      logPhase("UPGRADE");
    }
    if (phase_ == Phase::Upgrade) {
      if (upgradeSent_ < upgradeLength_) {
        const int n = esp_tls_conn_write(tls_, upgrade_ + upgradeSent_, upgradeLength_ - upgradeSent_);
        if (n > 0) upgradeSent_ += n; else if (!wouldBlock(n)) { fail(-7); return; }
        if (upgradeSent_ < upgradeLength_) return;
      }
      // Read only through the HTTP header terminator. Frames arriving in the
      // same TLS record remain available for the normal parser, never discarded.
      for (unsigned i = 0; i < 256; ++i) {
        char c; const int n = esp_tls_conn_read(tls_, &c, 1);
        if (n < 0 && wouldBlock(n)) return;
        if (n <= 0 || upgradeUsed_ + 1 >= sizeof(upgrade_)) { fail(-8); return; }
        upgrade_[upgradeUsed_++] = c; upgrade_[upgradeUsed_] = 0;
        if (upgradeUsed_ >= 4 && !strcmp(upgrade_ + upgradeUsed_ - 4, "\r\n\r\n")) {
          const bool valid = validUpgrade(); logUpgradeResult(valid ? 1 : 0);
          if (!valid) { fail(-9); return; }
          delete admission_; admission_ = nullptr; phase_ = Phase::Open;
          logPhase("OPEN");
          lastRx_ = lastTx_ = now; memset(upgrade_, 0, sizeof(upgrade_)); break;
        }
      }
      if (phase_ != Phase::Open) return;
    }
    if (static_cast<uint32_t>(now - lastRx_) > 90000U ||
        (txCount_ && static_cast<uint32_t>(now - txProgressAt_) > 10000U)) { fail(-10); return; }
    if (static_cast<uint32_t>(now - lastTx_) >= 30000U && !txCount_) enqueue(9, nullptr, 0);
    // At most four sends/4096 bytes and 1024 receive bytes per tick. The bounded
    // batch drains multi-part config reports without starving their last part.
    size_t sent = 0;
    for (unsigned count = 0; txCount_ && count < 4 && sent < 4096; ++count) {
      Slot &slot = tx_[txHead_];
      const size_t wanted = min<size_t>(slot.length - txOffset_, 4096 - sent);
      const int n = esp_tls_conn_write(tls_, slot.bytes + txOffset_, wanted);
      if (n > 0) { txOffset_ += n; sent += n; lastTx_ = txProgressAt_ = now;
        if (txOffset_ == slot.length) { txOffset_ = 0; txHead_ = (txHead_ + 1) % QUEUE_CAP; --txCount_; } }
      else if (!wouldBlock(n)) { fail(-11); return; }
      else break;
    }
    uint8_t input[1024]; const int n = esp_tls_conn_read(tls_, input, sizeof(input));
    if (n > 0) {
      lastRx_ = now;
      if (!reader_.push(input, n, [&](uint8_t opcode, const uint8_t *data, size_t length) {
        if (opcode == 8) return false;
        if (opcode == 9) return enqueue(10, data, length);
        if (opcode == 1 && callback_) callback_(data, length);
        return true;
      })) fail(-12);
    } else if (!wouldBlock(n)) fail(-13);
  }
 private:
  enum class Phase : uint8_t { Closed, Dns, Tls, Upgrade, Open };
  static constexpr uint8_t QUEUE_CAP = 8;
  struct Slot { uint8_t bytes[MayapWebSocket::FRAME_CAP + 8]; uint16_t length = 0; };
  // Diagnostics stay on the realtime owner, use the bounded Serial mailbox,
  // and never log request/response buffers or run inside the DNS callback.
  static void logPhase(const char *phase) {
#if MAYAP_DIAGNOSTIC_SERIAL
    mayapSerialPrintf(false, "[WS-CONNECT] phase=%s\n", phase);
#else
    (void)phase;
#endif
  }
  void logTlsState() {
#if MAYAP_DIAGNOSTIC_SERIAL
    esp_tls_conn_state_t state;
    if (!tls_ || esp_tls_get_conn_state(tls_, &state) != ESP_OK) return;
    if (static_cast<int>(state) == lastTlsState_) return;
    lastTlsState_ = static_cast<int>(state);
    // The real state machine advances monotonically through at most five states.
    // Also bound output if a faulty HAL unexpectedly oscillates states.
    if (tlsStateLogs_ >= 5U) return;
    ++tlsStateLogs_;
    mayapSerialPrintf(false, "[WS-CONNECT] tls_conn_state=%d\n", lastTlsState_);
#endif
  }
  void logTimeout(uint32_t now) {
#if MAYAP_DIAGNOSTIC_SERIAL
    esp_tls_conn_state_t state = ESP_TLS_INIT;
    const int tlsState = tls_ && esp_tls_get_conn_state(tls_, &state) == ESP_OK ? static_cast<int>(state) : -1;
    const char *phase = phase_ == Phase::Dns ? "DNS" : phase_ == Phase::Tls ? "TLS" : "UPGRADE";
    mayapSerialPrintf(false,
        "[WS-CONNECT] timeout elapsed_ms=%lu phase=%s dnsReady=%u tls_conn_state=%d upgradeSent=%u upgradeUsed=%u\n",
        static_cast<unsigned long>(now - started_), phase,
        static_cast<unsigned>(__atomic_load_n(&dnsReady_, __ATOMIC_ACQUIRE)), tlsState,
        static_cast<unsigned>(upgradeSent_), static_cast<unsigned>(upgradeUsed_));
    if (tlsState == ESP_TLS_CONNECTING) {
      int fd = -1;
      const int fdResult = esp_tls_get_conn_sockfd(tls_, &fd);
      int result = -1, error = 0;
      if (fdResult == ESP_OK && fd >= 0) {
        sockaddr_storage peer{}; socklen_t size = sizeof(peer);
        result = getpeername(fd, reinterpret_cast<sockaddr *>(&peer), &size);
        if (result < 0) error = errno;
      }
      mayapSerialPrintf(false, "[WS-CONNECT] peer_established=%u fd_result=%d result=%d errno=%d\n",
          result == 0 ? 1U : 0U, fdResult, result, error);
    }
#else
    (void)now;
#endif
  }
  void logUpgradeResult(int validation) {
#if MAYAP_DIAGNOSTIC_SERIAL
    if (upgradeResultLogged_) return;
    upgradeResultLogged_ = true;
    unsigned status = 0;
    // Read only the numeric status, even on partial/error responses. The same
    // buffer previously held Authorization; never print it as a string.
    if (upgradeUsed_ >= 12 && !strncmp(upgrade_, "HTTP/1.", 7) && upgrade_[8] == ' ' &&
        upgrade_[9] >= '0' && upgrade_[9] <= '9' && upgrade_[10] >= '0' && upgrade_[10] <= '9' &&
        upgrade_[11] >= '0' && upgrade_[11] <= '9')
      status = (upgrade_[9] - '0') * 100U + (upgrade_[10] - '0') * 10U + upgrade_[11] - '0';
    // status=0: unavailable; validation=-1: complete headers were not validated.
    mayapSerialPrintf(false, "[WS-CONNECT] http_status=%u websocket_valid=%d\n", status, validation);
#else
    (void)validation;
#endif
  }
  static bool wouldBlock(int n) { return n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE; }
  static void dnsDone(const char *, const ip_addr_t *address, void *arg) {
    auto *self = static_cast<WebSocketTransport *>(arg);
    if (address && IP_IS_V4(address)) {
      const auto *v4 = ip_2_ip4(address);
      self->dnsAddress_[0] = ip4_addr1(v4); self->dnsAddress_[1] = ip4_addr2(v4);
      self->dnsAddress_[2] = ip4_addr3(v4); self->dnsAddress_[3] = ip4_addr4(v4);
      __atomic_store_n(&self->dnsReady_, 1U, __ATOMIC_RELEASE);
    } else __atomic_store_n(&self->dnsReady_, 2U, __ATOMIC_RELEASE);
    __atomic_store_n(&self->dnsPending_, 0U, __ATOMIC_RELEASE);
  }
  bool enqueue(uint8_t opcode, const uint8_t *data, size_t length) {
    if (txCount_ == QUEUE_CAP) return false;
    Slot &slot = tx_[txTail_];
    slot.length = MayapWebSocket::clientFrame(slot.bytes, sizeof(slot.bytes), opcode, data, length, esp_random());
    if (!slot.length) return false;
    if (!txCount_) txProgressAt_ = millis();
    txTail_ = (txTail_ + 1) % QUEUE_CAP; ++txCount_; return true;
  }
  bool validUpgrade() {
    if (strncmp(upgrade_, "HTTP/1.1 101 ", 13)) return false;
    bool accepted = false, upgrade = false, connection = false;
    char *line = strstr(upgrade_, "\r\n");
    while (line && line[2]) {
      line += 2; char *end = strstr(line, "\r\n"); if (!end) return false;
      *end = 0; char *colon = strchr(line, ':');
      if (colon) { *colon = 0; char *value = colon + 1; while (*value == ' ' || *value == '\t') ++value;
        if (!strcasecmp(line, "Sec-WebSocket-Accept")) accepted = !strcmp(value, accept_);
        else if (!strcasecmp(line, "Upgrade")) upgrade = !strcasecmp(value, "websocket");
        else if (!strcasecmp(line, "Connection")) connection = !strcasecmp(value, "Upgrade"); }
      *end = '\r'; line = end;
    }
    return accepted && upgrade && connection;
  }
  void fail(int error) {
    if (phase_ == Phase::Upgrade) logUpgradeResult(-1);
    disconnect(); error_ = error;
  }
  esp_tls_t *tls_ = nullptr; esp_tls_cfg_t cfg_ = {};
  MayapTlsOperation *admission_ = nullptr; Callback callback_ = nullptr;
  Phase phase_ = Phase::Closed; int error_ = 0;
#if MAYAP_DIAGNOSTIC_SERIAL
  int lastTlsState_ = -1;
  uint8_t tlsStateLogs_ = 0;
  bool upgradeResultLogged_ = false;
#endif
  char host_[128] = {}, ip_[16] = {}, accept_[29] = {}, upgrade_[1024] = {};
  size_t upgradeLength_ = 0, upgradeSent_ = 0, upgradeUsed_ = 0, txOffset_ = 0;
  uint32_t started_ = 0, lastRx_ = 0, lastTx_ = 0, txProgressAt_ = 0;
  uint8_t dnsAddress_[4] = {}, dnsReady_ = 0, dnsPending_ = 0;
  uint8_t txHead_ = 0, txTail_ = 0, txCount_ = 0;
  Slot tx_[QUEUE_CAP]; MayapWebSocket::Reader reader_;
};
