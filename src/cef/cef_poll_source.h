#pragma once

#include "app/poll_source.h"
#include "cef/cef_message_pump_deadline.h"

#include <cstdint>
#include <memory>

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
  struct State {
    CefMessagePumpDeadline deadline;
  };

  // Called (possibly from a CEF thread) when the next pump time changes.
  static void scheduleWork(const std::shared_ptr<State>& state, std::int64_t delayMs);

  CefService& m_service;
  // CEF can enter its scheduling callback from another thread while the
  // application is tearing down this poll source. The callback retains only
  // this deadline state, never a pointer to the CefPollSource object.
  std::shared_ptr<State> m_state = std::make_shared<State>();
};
