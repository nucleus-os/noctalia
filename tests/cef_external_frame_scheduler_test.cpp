#include "cef/cef_external_frame_scheduler.h"

#include <cassert>
#include <cstdint>

namespace {

constexpr std::int64_t k120Hz = 8'333'333;
constexpr std::int64_t k60Hz = 16'666'667;
constexpr std::int64_t k144Hz = 6'944'444;

SurfacePresentationFeedback presentation(
    std::int64_t presentedNs, std::uint64_t sequence, std::uint32_t refreshNs = k120Hz
) {
  return SurfacePresentationFeedback{
      .presentedSteadyNs = presentedNs,
      .sequence = sequence,
      .refreshNs = refreshNs,
      .presented = true,
      .exactClock = true,
  };
}

} // namespace

int main() {
  CefExternalFrameScheduler scheduler(k120Hz);
  assert(scheduler.state() == CefExternalFrameScheduler::State::Suspended);
  assert(!scheduler.needsFrameOpportunity());
  assert(!scheduler.onFrameOpportunity(1));

  scheduler.resume();
  assert(scheduler.needsFrameOpportunity());
  const auto first = scheduler.onFrameOpportunity(100'000'000);
  assert(first);
  assert(first->intervalNs == k120Hz);
  assert(first->deadlineDeltaNs == k120Hz);
  assert(scheduler.state() == CefExternalFrameScheduler::State::InFlight);

  // Multiple opportunities coalesce behind the real Chromium acknowledgment.
  assert(!scheduler.onFrameOpportunity(108'333'333));
  assert(!scheduler.onFrameOpportunity(116'666'666));
  const auto second = scheduler.acknowledge(first->id, true, 117'000'000);
  assert(second);
  assert(second->id != first->id);

  // Stale acknowledgments cannot complete a newer request.
  assert(!scheduler.acknowledge(first->id, true, 118'000'000));
  assert(scheduler.state() == CefExternalFrameScheduler::State::InFlight);
  assert(!scheduler.acknowledge(second->id, true, 118'000'000));
  assert(scheduler.state() == CefExternalFrameScheduler::State::Idle);

  // Presentation phase, including exact 120 Hz nanoseconds, drives deadlines.
  scheduler.onPresentation(presentation(200'000'000, 10));
  const auto phased = scheduler.onFrameOpportunity(204'000'000);
  assert(phased);
  assert(phased->deadlineDeltaNs == 4'333'333);
  assert(phased->intervalNs == k120Hz);
  assert(!scheduler.acknowledge(phased->id, true, 205'000'000));

  // Older feedback cannot move the phase backwards; a refresh change can.
  scheduler.onPresentation(presentation(190'000'000, 9, k60Hz));
  assert(scheduler.intervalNs() == k120Hz);
  scheduler.onPresentation(presentation(210'000'000, 11, k144Hz));
  assert(scheduler.intervalNs() == k144Hz);

  // A delayed acknowledgment does not fabricate intermediate requests. The
  // newest compositor opportunity becomes one request at the next 144 Hz
  // presentation phase.
  const auto delayed = scheduler.onFrameOpportunity(211'000'000);
  assert(delayed);
  assert(delayed->intervalNs == k144Hz);
  assert(!scheduler.onFrameOpportunity(217'944'444));
  assert(!scheduler.onFrameOpportunity(224'888'888));
  const auto afterDelay = scheduler.acknowledge(delayed->id, true, 227'000'000);
  assert(afterDelay);
  assert(afterDelay->intervalNs == k144Hz);
  assert(afterDelay->deadlineDeltaNs == 3'833'332);
  assert(!scheduler.acknowledge(afterDelay->id, true, 228'000'000));

  // Refresh changes preserve their exact nanosecond intervals instead of
  // rounding through an integer FPS value.
  scheduler.onPresentation(presentation(240'000'000, 12, k60Hz));
  const auto sixty = scheduler.onFrameOpportunity(241'000'000);
  assert(sixty);
  assert(sixty->intervalNs == k60Hz);
  assert(sixty->deadlineDeltaNs == 15'666'667);
  assert(!scheduler.acknowledge(sixty->id, true, 242'000'000));

  // Urgency survives opportunity coalescing and wakes an idle-suppressed page.
  for (std::uint32_t i = 0; i < CefExternalFrameScheduler::kIdleNoDamageThreshold; ++i) {
    const auto quiet = scheduler.onFrameOpportunity(220'000'000 + i * 7'000'000);
    assert(quiet);
    assert(!scheduler.acknowledge(quiet->id, false, 221'000'000 + i * 7'000'000));
  }
  assert(scheduler.idleSuppressed());
  assert(!scheduler.needsFrameOpportunity());
  assert(!scheduler.onFrameOpportunity(260'000'000));
  const auto urgent = scheduler.requestUrgent(261'000'000);
  assert(urgent && urgent->urgent);
  assert(!scheduler.idleSuppressed());
  assert(scheduler.needsFrameOpportunity());

  // Detach invalidates the in-flight generation and ignores its late ack.
  const auto oldGeneration = urgent->generation;
  scheduler.suspend();
  assert(!scheduler.needsFrameOpportunity());
  assert(!scheduler.acknowledge(urgent->id, true, 262'000'000));
  scheduler.resume();
  const auto afterResume = scheduler.requestNormal(263'000'000);
  assert(afterResume);
  assert(afterResume->generation != oldGeneration);

  // A new surface/output can restart presentation sequence numbering. The
  // lifecycle boundary must discard the old output phase while preserving the
  // most recently learned interval until fresh feedback arrives.
  assert(afterResume->intervalNs == k60Hz);
  assert(afterResume->deadlineDeltaNs == k60Hz);
  assert(!scheduler.acknowledge(afterResume->id, true, 264'000'000));
  scheduler.onPresentation(presentation(270'000'000, 1, k144Hz));
  const auto newOutput = scheduler.onFrameOpportunity(271'000'000);
  assert(newOutput);
  assert(newOutput->intervalNs == k144Hz);
  assert(newOutput->deadlineDeltaNs == 5'944'444);

  // A rejected request can be abandoned without accepting a late callback.
  assert(scheduler.abandon(newOutput->id));
  assert(scheduler.state() == CefExternalFrameScheduler::State::Idle);
  assert(scheduler.needsFrameOpportunity());
  assert(!scheduler.acknowledge(newOutput->id, true, 272'000'000));

  // Idle probes bypass suppression exactly once and real damage restores the
  // compositor-paced active path.
  for (std::uint32_t i = 0; i < CefExternalFrameScheduler::kIdleNoDamageThreshold; ++i) {
    const auto quiet = scheduler.onFrameOpportunity(300'000'000 + i * 7'000'000);
    assert(quiet);
    assert(!scheduler.acknowledge(quiet->id, false, 301'000'000 + i * 7'000'000));
  }
  const auto probe = scheduler.requestIdleProbe(340'000'000);
  assert(probe);
  assert(!scheduler.needsFrameOpportunity());
  const auto probeGeneration = probe->generation;
  assert(!scheduler.acknowledge(probe->id, true, 341'000'000));
  assert(!scheduler.idleSuppressed());
  assert(scheduler.needsFrameOpportunity());
  const auto restarted = scheduler.onFrameOpportunity(347'000'000);
  assert(restarted);
  assert(restarted->generation == probeGeneration);
  assert(!scheduler.acknowledge(restarted->id, false, 348'000'000));

  // A no-damage idle probe remains quiescent and does not accidentally restart
  // the compositor callback chain.
  for (std::uint32_t i = scheduler.consecutiveNoDamage();
       i < CefExternalFrameScheduler::kIdleNoDamageThreshold; ++i) {
    const auto quiet = scheduler.onFrameOpportunity(350'000'000 + i * 7'000'000);
    assert(quiet);
    assert(!scheduler.acknowledge(quiet->id, false, 351'000'000 + i * 7'000'000));
  }
  assert(scheduler.idleSuppressed());
  const auto quietProbe = scheduler.requestIdleProbe(390'000'000);
  assert(quietProbe);
  assert(!scheduler.acknowledge(quietProbe->id, false, 391'000'000));
  assert(scheduler.idleSuppressed());
  assert(!scheduler.needsFrameOpportunity());

  // Urgent input arriving behind an idle probe is sticky. The probe's
  // no-damage acknowledgment must immediately produce the urgent successor.
  const auto busyProbe = scheduler.requestIdleProbe(400'000'000);
  assert(busyProbe);
  assert(!scheduler.requestUrgent(401'000'000));
  const auto urgentAfterProbe =
      scheduler.acknowledge(busyProbe->id, false, 402'000'000);
  assert(urgentAfterProbe);
  assert(urgentAfterProbe->urgent);
  assert(!scheduler.idleSuppressed());
  assert(scheduler.needsFrameOpportunity());
}
