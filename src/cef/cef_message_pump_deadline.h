#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>

// Thread-safe deadline coalescing for CEF's external message pump. CEF may
// schedule from any thread; the main poll loop always observes the earliest
// outstanding absolute deadline.
class CefMessagePumpDeadline {
public:
  static constexpr std::int64_t kNone = -1;

  [[nodiscard]] bool schedule(
      std::int64_t nowMs, std::int64_t delayMs
  ) noexcept {
    const std::int64_t clamped = std::max<std::int64_t>(0, delayMs);
    const std::int64_t requested =
        clamped > std::numeric_limits<std::int64_t>::max() - nowMs
        ? std::numeric_limits<std::int64_t>::max()
        : nowMs + clamped;

    std::int64_t current = m_deadlineMs.load();
    while ((current == kNone || requested < current)
           && !m_deadlineMs.compare_exchange_weak(current, requested)) {
    }
    return current == kNone || requested < current;
  }

  [[nodiscard]] int pollTimeoutMs(std::int64_t nowMs) const noexcept {
    const std::int64_t deadline = m_deadlineMs.load();
    if (deadline == kNone) {
      return -1;
    }
    const std::int64_t remaining = deadline - nowMs;
    if (remaining <= 0) {
      return 0;
    }
    return static_cast<int>(std::min<std::int64_t>(remaining, 1000));
  }

  [[nodiscard]] std::optional<std::int64_t> claimDue(
      std::int64_t nowMs
  ) noexcept {
    std::int64_t deadline = m_deadlineMs.load();
    if (deadline == kNone || deadline > nowMs) {
      return std::nullopt;
    }
    if (!m_deadlineMs.compare_exchange_strong(deadline, kNone)) {
      return std::nullopt;
    }
    return deadline;
  }

  [[nodiscard]] std::int64_t deadlineMs() const noexcept {
    return m_deadlineMs.load();
  }

private:
  std::atomic<std::int64_t> m_deadlineMs{kNone};
};
