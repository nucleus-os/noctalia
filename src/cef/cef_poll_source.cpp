#include "cef/cef_poll_source.h"

#include "cef/cef_service.h"
#include "core/deferred_call.h"
#include "core/tracy_latency.h"

#include <chrono>

namespace {
constexpr std::int64_t kMaxPumpDelayMs = 1000 / 30;

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
} // namespace

CefPollSource::CefPollSource(CefService& service) : m_service(service) {
  m_service.setScheduleWorkCallback(
      [state = std::weak_ptr<State>(m_state)](std::int64_t delayMs) {
        if (const auto retained = state.lock()) {
          scheduleWork(retained, delayMs);
        }
      }
  );
}

void CefPollSource::scheduleWork(
    const std::shared_ptr<State>& state, std::int64_t delayMs
) {
  const bool deadlineUpdated = state->deadline.schedule(nowMs(), delayMs);
  tracy_latency::messagePumpScheduled(delayMs, deadlineUpdated);
  // Wake the poll loop so it re-reads pollTimeoutMs() with the new deadline.
  DeferredCall::callLater([]() {});
}

int CefPollSource::pollTimeoutMs() const {
  return m_state->deadline.pollTimeoutMs(nowMs());
}

void CefPollSource::doAddPollFds(std::vector<pollfd>& /*fds*/) {
  // No fds in external-message-pump mode.
}

void CefPollSource::dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) {
  const long long dispatchNowMs = nowMs();
  const auto deadline = m_state->deadline.claimDue(dispatchNowMs);
  if (!deadline) {
    return;
  }
  tracy_latency::messagePumpDispatched(
      std::max<long long>(0, dispatchNowMs - *deadline)
  );
  m_service.doMessageLoopWork();
  // CEF's reference external-pump implementation requires a bounded periodic
  // pump even when OnScheduleMessagePumpWork emits no follow-up request.
  scheduleWork(m_state, kMaxPumpDelayMs);
}
