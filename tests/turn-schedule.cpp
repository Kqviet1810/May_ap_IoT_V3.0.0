#include <cassert>
#include <cstdint>
#include <cstdio>
#include "../MAYAP_INDUSTRIAL_v1_0_0/turn_schedule_policy.h"

using MayapTurning::fromEpoch;

int main() {
  constexpr uint32_t anchor = 1000000U;

  // Normal schedule: 70 minutes elapsed in a 120-minute interval.
  auto s = fromEpoch(anchor + 70U * 60U, anchor, 120U);
  assert(s.scheduled && !s.due);
  assert(s.remainingMs == 50U * 60U * 1000U);

  // Power was absent for another 5 minutes: do not restart a fresh interval.
  s = fromEpoch(anchor + 75U * 60U, anchor, 120U);
  assert(s.scheduled && !s.due);
  assert(s.remainingMs == 45U * 60U * 1000U);

  // If the original deadline passed during the outage, the next turn is due now.
  s = fromEpoch(anchor + 121U * 60U, anchor, 120U);
  assert(s.scheduled && s.due && s.remainingMs == 0U);

  // Changing the interval reprojects from the last REAL turn, not from edit time.
  s = fromEpoch(anchor + 60U * 60U, anchor, 180U);
  assert(s.scheduled && !s.due);
  assert(s.remainingMs == 120U * 60U * 1000U);

  // No persistent anchor/current RTC means caller must use the bounded RAM fallback.
  assert(!fromEpoch(anchor, 0U, 120U).scheduled);
  assert(!fromEpoch(0U, anchor, 120U).scheduled);

  // Long intervals are bounded for signed millis comparisons.
  s = fromEpoch(1U, 1U, 65535U);
  assert(s.scheduled && !s.due && s.remainingMs == 0x7FFFFFFFU);

  std::puts("Turning schedule policy: persistent anchor survives outage/reprojection PASS");
}
