#pragma once
#include <stddef.h>

namespace MayapProtocol {
constexpr size_t MQTT_HARD_CAP = 4096U;
constexpr size_t MQTT_NORMAL_CAP = 2048U;
constexpr size_t MQTT_SMALL_TARGET = 512U;
constexpr size_t MQTT_CHUNK_TARGET = 1024U;
constexpr size_t MQTT_OVERHEAD = 5U;
constexpr size_t MQTT_SIGNED_BODY_CAP = 1700U;
}
