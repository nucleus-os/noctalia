#include "cef/cef_poll_source.h"

#include "cef/cef_service.h"
#include "core/deferred_call.h"
#include "core/tracy_latency.h"

#include <algorithm>
#include <chrono>

namespace {
constexpr std::int64_t kMaxPumpDelayMs = 1000 / 30;

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
  const long long requested = nowMs() + clamped;
  long long current = m_deadlineMs.load();
  bool deadlineUpdated = false;
  while ((current == kNone || requested < current)
         && !m_deadlineMs.compare_exchange_weak(current, requested)) {
  }
  deadlineUpdated = current == kNone || requested < current;
  tracy_latency::messagePumpScheduled(delayMs, deadlineUpdated);
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
  long long deadline = m_deadlineMs.load();
  if (deadline == kNone) {
    return;
  }
  if (deadline - nowMs() > 0) {
    return; // not due yet (woke for another source)
  }
  // Claim this deadline before pumping. If another thread schedules earlier
  // work after this exchange it remains visible for the next dispatch.
  if (!m_deadlineMs.compare_exchange_strong(deadline, kNone)) {
    return;
  }
  tracy_latency::messagePumpDispatched(std::max<long long>(0, nowMs() - deadline));
  m_service.doMessageLoopWork();
  // CEF's reference external-pump implementation requires a bounded periodic
  // pump even when OnScheduleMessagePumpWork emits no follow-up request.
  scheduleWork(kMaxPumpDelayMs);
}
