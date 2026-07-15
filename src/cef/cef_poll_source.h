#pragma once

#include "app/poll_source.h"

#include <atomic>
#include <cstdint>

class CefService;

// Drives CEF's external message pump from noctalia's poll loop. CEF calls
// OnScheduleMessagePumpWork(delayMs) whenever its next work is due; we store an
// absolute deadline and advertise it via pollTimeoutMs() so the loop wakes at
// the right time and dispatch() runs CefDoMessageLoopWork(). No fds — CEF has
// no single pollable fd in this mode.
class CefPollSource final : public PollSource {
public:
  explicit CefPollSource(CefService& service);

  [[nodiscard]] int pollTimeoutMs() const override;
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  // Called (possibly from a CEF thread) when the next pump time changes.
  void scheduleWork(std::int64_t delayMs);

  CefService& m_service;
  // Absolute deadline in steady-clock milliseconds; kNone when idle.
  static constexpr long long kNone = -1;
  std::atomic<long long> m_deadlineMs{kNone};
};
