#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
struct Subscription { std::string topic; uint8_t qos; };
struct FakeMqtt {
  std::vector<Subscription> sent;
  unsigned failAt = 0U;
  bool subscribe(const char *topic, uint8_t qos = 0) {
    sent.push_back({topic, qos});
    return sent.size() != failAt;
  }
} mqtt;
static const char *topicOf(const char *suffix) { return suffix; }
#include "actual-mqtt-subscriptions.inc"
int main() {
  const char *topics[] = {"config/set", "reminders/set", "command", "history/request", "session"};
  for (unsigned reconnect = 0; reconnect < 100; ++reconnect) {
    mqtt.sent.clear();
    assert(subscribeAll() && mqtt.sent.size() == 5U);
    for (unsigned i = 0; i < 5; ++i) {
      assert(mqtt.sent[i].topic == topics[i]);
      assert(mqtt.sent[i].qos == (i == 4U ? 0U : 1U));
    }
  }
  for (unsigned failure = 1; failure <= 5; ++failure) {
    mqtt.sent.clear(); mqtt.failAt = failure;
    assert(!subscribeAll() && mqtt.sent.size() == 5U);
  }
  std::puts("Actual MQTT: 100 clean reconnect subscriptions stay QoS1; every send failure is reported PASS");
}
