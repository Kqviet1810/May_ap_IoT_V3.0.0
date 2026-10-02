#pragma once
#include <stddef.h>

namespace MayapProtocol {
constexpr size_t FRAME_HARD_CAP = 4096U;
constexpr size_t FRAME_NORMAL_CAP = 2048U;
constexpr size_t FRAME_SMALL_TARGET = 512U;
constexpr size_t FRAME_CHUNK_TARGET = 1024U;
constexpr size_t FRAME_OVERHEAD = 80U;
constexpr size_t FRAME_SIGNED_BODY_CAP = 1700U;
}
