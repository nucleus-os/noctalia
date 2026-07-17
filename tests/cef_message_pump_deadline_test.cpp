#include "cef/cef_message_pump_deadline.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

int main() {
  CefMessagePumpDeadline deadline;
  assert(deadline.pollTimeoutMs(1000) == -1);
  assert(!deadline.claimDue(1000));

  assert(deadline.schedule(1000, 50));
  assert(deadline.deadlineMs() == 1050);
  assert(deadline.pollTimeoutMs(1000) == 50);
  assert(!deadline.claimDue(1049));

  // A later request must not postpone already scheduled work.
  assert(!deadline.schedule(1000, 100));
  assert(deadline.deadlineMs() == 1050);

  // An earlier and a non-positive request replace the deadline.
  assert(deadline.schedule(1000, 20));
  assert(deadline.deadlineMs() == 1020);
  assert(deadline.schedule(1000, -5));
  assert(deadline.deadlineMs() == 1000);
  const auto claimed = deadline.claimDue(1003);
  assert(claimed && *claimed == 1000);
  assert(deadline.pollTimeoutMs(1003) == -1);

  // Poll waits are bounded even when CEF asks for a very distant deadline,
  // and addition saturates rather than overflowing into an immediate wakeup.
  assert(deadline.schedule(2000, 10'000));
  assert(deadline.pollTimeoutMs(2000) == 1000);
  assert(deadline.claimDue(12'000));
  assert(deadline.schedule(
      std::numeric_limits<std::int64_t>::max() - 5, 100
  ));
  assert(deadline.deadlineMs() == std::numeric_limits<std::int64_t>::max());
  assert(!deadline.claimDue(std::numeric_limits<std::int64_t>::max() - 1));
  assert(deadline.claimDue(std::numeric_limits<std::int64_t>::max()));

  // Concurrent producers converge on the earliest absolute deadline.
  constexpr std::int64_t kNow = 50'000;
  const std::vector<std::int64_t> delays{80, 30, 70, 10, 60, 20, 50, 40};
  std::vector<std::thread> workers;
  workers.reserve(delays.size());
  for (const std::int64_t delay : delays) {
    workers.emplace_back([&deadline, delay]() {
      (void)deadline.schedule(kNow, delay);
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  assert(deadline.deadlineMs() == kNow + 10);
  assert(deadline.pollTimeoutMs(kNow) == 10);
}
