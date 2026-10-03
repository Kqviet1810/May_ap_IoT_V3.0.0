#include <cassert>
#include <cstdint>
#include <cstdio>
#include "notes_retry_policy.h"

int main() {
  using namespace MayapNotes;
  ReconcileBudget budget;
  assert(!budget.active());

  budget.begin(1000U);
  assert(budget.active() && budget.attempts() == 0U && !budget.exhausted(1000U));
  assert(!budget.due(2999U, 1000U));
  assert(budget.due(3000U, 1000U));

  for (uint8_t i = 0; i < RECONCILE_MAX_ATTEMPTS - 1U; ++i) {
    budget.markAttempt();
    assert(!budget.exhausted(3000U + i));
  }
  budget.markAttempt();
  assert(budget.exhausted(5000U));

  budget.reset();
  assert(!budget.active() && !budget.exhausted(5000U));

  const uint32_t start = UINT32_MAX - 5000U;
  budget.begin(start);
  assert(!budget.exhausted(static_cast<uint32_t>(start + RECONCILE_MAX_WINDOW_MS - 1U)));
  assert(budget.exhausted(static_cast<uint32_t>(start + RECONCILE_MAX_WINDOW_MS)));

  std::puts("Notes retry policy: finite attempts, deadline and millis wrap PASS");
}
