#pragma once
#include <PubSubClient.h>

// Sole PubSubClient boundary. Realtime business logic uses this interface;
// another transport can implement the same methods without changing handlers.
class MqttTransport {
 public:
  explicit MqttTransport(Client &socket) : client_(socket) {}
  bool connected() { return client_.connected(); }
  void disconnect() { client_.disconnect(); }
  bool loop() { return client_.loop(); }
  int state() { return client_.state(); }
  bool subscribe(const char *topic, uint8_t qos = 0) { return client_.subscribe(topic, qos); }
  bool publish(const char *topic, const uint8_t *data, unsigned int length, bool retain) {
    return client_.publish(topic, data, length, retain);
  }
  bool connect(const char *id, const char *user, const char *password,
               const char *willTopic, uint8_t willQos, bool willRetain,
               const char *willMessage, bool cleanSession = true) {
    return client_.connect(id, user, password, willTopic, willQos, willRetain, willMessage, cleanSession);
  }
  bool setBufferSize(uint16_t size) { return client_.setBufferSize(size); }
  void setServer(const char *host, uint16_t port) { client_.setServer(host, port); }
  void setCallback(MQTT_CALLBACK_SIGNATURE) { client_.setCallback(callback); }
  void setKeepAlive(uint16_t seconds) { client_.setKeepAlive(seconds); }
  void setSocketTimeout(uint16_t seconds) { client_.setSocketTimeout(seconds); }
 private:
  PubSubClient client_;
};
