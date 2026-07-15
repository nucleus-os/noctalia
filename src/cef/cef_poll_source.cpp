#include "cef/cef_poll_source.h"

#include "cef/cef_service.h"
#include "core/deferred_call.h"

#include <algorithm>
#include <chrono>

namespace {
long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
} // namespace

CefPollSource::CefPollSource(CefService& service) : m_service(service) {
  m_service.setScheduleWorkCallback([this](std::int64_t delayMs) { scheduleWork(delayMs); });
}

void CefPollSource::scheduleWork(std::int64_t delayMs) {
  const long long clamped = std::max<std::int64_t>(0, delayMs);
  m_deadlineMs.store(nowMs() + clamped);
  // Wake the poll loop so it re-reads pollTimeoutMs() with the new deadline.
  DeferredCall::callLater([]() {});
}

int CefPollSource::pollTimeoutMs() const {
  const long long deadline = m_deadlineMs.load();
  if (deadline == kNone) {
    return -1;
  }
  const long long remaining = deadline - nowMs();
  if (remaining <= 0) {
    return 0;
  }
  return static_cast<int>(std::min<long long>(remaining, 1000));
}

void CefPollSource::doAddPollFds(std::vector<pollfd>& /*fds*/) {
  // No fds in external-message-pump mode.
}

void CefPollSource::dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) {
  const long long deadline = m_deadlineMs.load();
  if (deadline == kNone) {
    return;
  }
  if (deadline - nowMs() > 0) {
    return; // not due yet (woke for another source)
  }
  // Clear before pumping so work scheduled during the pump is not lost.
  m_deadlineMs.store(kNone);
  m_service.doMessageLoopWork();
}
