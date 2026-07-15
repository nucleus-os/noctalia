#pragma once

#include "core/tracy.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace tracy_latency {

enum class InputKind : std::int64_t {
  PointerMove = 1,
  PointerButton = 2,
  PointerWheel = 3,
  Key = 4,
};

struct PresentationTrace {
  std::uint64_t sequence = 0;
  std::int64_t kind = 0;
  std::int64_t inputNs = 0;
  std::int64_t paintNs = 0;
  std::int64_t redrawNs = 0;
  std::int64_t queuePresentNs = 0;

  [[nodiscard]] bool valid() const noexcept { return sequence != 0; }
};

#ifdef NOCTALIA_TRACY_ENABLE

namespace detail {

struct State {
  std::atomic<std::uint64_t> nextSequence{0};
  std::atomic<std::uint64_t> pendingInputSequence{0};
  std::atomic<std::int64_t> pendingInputKind{0};
  std::atomic<std::int64_t> inputReceivedNs{0};
  std::atomic<std::int64_t> inputForwardedNs{0};
  std::atomic<std::int64_t> firstPumpRequestNs{0};
  std::atomic<std::int64_t> firstAcceptedPumpRequestNs{0};
  std::atomic<std::int64_t> firstPumpDispatchNs{0};
  std::atomic<std::int64_t> latestPumpDispatchNs{0};
  std::atomic<std::uint64_t> pumpDispatchesSinceInput{0};
  std::atomic<std::int64_t> immediateExternalBeginFrameNs{0};
  std::atomic<std::int64_t> latestExternalBeginFrameNs{0};
  std::atomic<std::uint64_t> externalBeginFramesSinceInput{0};
  std::atomic<std::uint64_t> presentationSequence{0};
  std::atomic<std::int64_t> presentationInputKind{0};
  std::atomic<std::int64_t> presentationInputNs{0};
  std::atomic<std::int64_t> acceleratedPaintNs{0};
  std::atomic<std::int64_t> redrawQueuedNs{0};
  std::atomic<std::uint64_t> coalescedInputs{0};
  std::atomic<std::uint64_t> supersededPaints{0};
};

inline State& state() {
  static State value;
  return value;
}

inline std::int64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()
  ).count();
}

inline double elapsedMs(std::int64_t startNs, std::int64_t endNs) {
  return startNs > 0 && endNs >= startNs ? static_cast<double>(endNs - startNs) / 1'000'000.0 : 0.0;
}

inline std::uint64_t latestSequence(const State& value) {
  const std::uint64_t input = value.pendingInputSequence.load(std::memory_order_acquire);
  return input != 0 ? input : value.presentationSequence.load(std::memory_order_acquire);
}

inline InputKind inputKind(std::int64_t raw) {
  return static_cast<InputKind>(raw);
}

inline int inputPriority(InputKind kind) {
  switch (kind) {
    case InputKind::PointerMove:
      return 1;
    case InputKind::PointerWheel:
      return 2;
    case InputKind::PointerButton:
    case InputKind::Key:
      return 3;
  }
  return 0;
}

inline void plotInputToPaint(InputKind kind, double elapsed) {
  switch (kind) {
    case InputKind::PointerMove:
      NOCTALIA_TRACE_PLOT("Latency motion input to accelerated paint ms", elapsed);
      break;
    case InputKind::PointerButton:
      NOCTALIA_TRACE_PLOT("Latency button input to accelerated paint ms", elapsed);
      break;
    case InputKind::PointerWheel:
      NOCTALIA_TRACE_PLOT("Latency wheel input to accelerated paint ms", elapsed);
      break;
    case InputKind::Key:
      NOCTALIA_TRACE_PLOT("Latency key input to accelerated paint ms", elapsed);
      break;
  }
}

inline void plotInputToPresent(InputKind kind, double elapsed) {
  switch (kind) {
    case InputKind::PointerMove:
      NOCTALIA_TRACE_PLOT("Latency motion input to present ms", elapsed);
      break;
    case InputKind::PointerButton:
      NOCTALIA_TRACE_PLOT("Latency button input to present ms", elapsed);
      break;
    case InputKind::PointerWheel:
      NOCTALIA_TRACE_PLOT("Latency wheel input to present ms", elapsed);
      break;
    case InputKind::Key:
      NOCTALIA_TRACE_PLOT("Latency key input to present ms", elapsed);
      break;
  }
}

inline void plotInputToVisible(InputKind kind, double elapsed) {
  switch (kind) {
    case InputKind::PointerMove:
      NOCTALIA_TRACE_PLOT("Latency motion input to visible ms", elapsed);
      break;
    case InputKind::PointerButton:
      NOCTALIA_TRACE_PLOT("Latency button input to visible ms", elapsed);
      break;
    case InputKind::PointerWheel:
      NOCTALIA_TRACE_PLOT("Latency wheel input to visible ms", elapsed);
      break;
    case InputKind::Key:
      NOCTALIA_TRACE_PLOT("Latency key input to visible ms", elapsed);
      break;
  }
}

inline void plotPaintToPresent(InputKind kind, double elapsed) {
  switch (kind) {
    case InputKind::PointerMove:
      NOCTALIA_TRACE_PLOT("Latency motion accelerated paint to present ms", elapsed);
      break;
    case InputKind::PointerButton:
      NOCTALIA_TRACE_PLOT("Latency button accelerated paint to present ms", elapsed);
      break;
    case InputKind::PointerWheel:
      NOCTALIA_TRACE_PLOT("Latency wheel accelerated paint to present ms", elapsed);
      break;
    case InputKind::Key:
      NOCTALIA_TRACE_PLOT("Latency key accelerated paint to present ms", elapsed);
      break;
  }
}

inline void plotForwardToPaint(InputKind kind, double elapsed) {
  switch (kind) {
    case InputKind::PointerMove:
      NOCTALIA_TRACE_PLOT("Latency motion CEF forward to accelerated paint ms", elapsed);
      break;
    case InputKind::PointerButton:
      NOCTALIA_TRACE_PLOT("Latency button CEF forward to accelerated paint ms", elapsed);
      break;
    case InputKind::PointerWheel:
      NOCTALIA_TRACE_PLOT("Latency wheel CEF forward to accelerated paint ms", elapsed);
      break;
    case InputKind::Key:
      NOCTALIA_TRACE_PLOT("Latency key CEF forward to accelerated paint ms", elapsed);
      break;
  }
}

inline void plotFirstDispatchToPaint(InputKind kind, double elapsed) {
  switch (kind) {
    case InputKind::PointerMove:
      NOCTALIA_TRACE_PLOT("Latency motion first CEF pump dispatch to accelerated paint ms", elapsed);
      break;
    case InputKind::PointerButton:
      NOCTALIA_TRACE_PLOT("Latency button first CEF pump dispatch to accelerated paint ms", elapsed);
      break;
    case InputKind::PointerWheel:
      NOCTALIA_TRACE_PLOT("Latency wheel first CEF pump dispatch to accelerated paint ms", elapsed);
      break;
    case InputKind::Key:
      NOCTALIA_TRACE_PLOT("Latency key first CEF pump dispatch to accelerated paint ms", elapsed);
      break;
  }
}

inline void plotPumpDispatchCount(InputKind kind, std::int64_t count) {
  switch (kind) {
    case InputKind::PointerMove:
      NOCTALIA_TRACE_PLOT("Latency motion CEF pump dispatch count before paint", count);
      break;
    case InputKind::PointerButton:
      NOCTALIA_TRACE_PLOT("Latency button CEF pump dispatch count before paint", count);
      break;
    case InputKind::PointerWheel:
      NOCTALIA_TRACE_PLOT("Latency wheel CEF pump dispatch count before paint", count);
      break;
    case InputKind::Key:
      NOCTALIA_TRACE_PLOT("Latency key CEF pump dispatch count before paint", count);
      break;
  }
}

inline void plotImmediateBeginFrameToPaint(InputKind kind, double elapsed) {
  switch (kind) {
    case InputKind::PointerMove:
      NOCTALIA_TRACE_PLOT("Latency motion immediate begin frame to accelerated paint ms", elapsed);
      break;
    case InputKind::PointerButton:
      NOCTALIA_TRACE_PLOT("Latency button immediate begin frame to accelerated paint ms", elapsed);
      break;
    case InputKind::PointerWheel:
      NOCTALIA_TRACE_PLOT("Latency wheel immediate begin frame to accelerated paint ms", elapsed);
      break;
    case InputKind::Key:
      NOCTALIA_TRACE_PLOT("Latency key immediate begin frame to accelerated paint ms", elapsed);
      break;
  }
}

} // namespace detail

inline void inputReceived(InputKind kind) {
  auto& state = detail::state();
  const std::int64_t pendingKind = state.pendingInputKind.load(std::memory_order_acquire);
  if (state.pendingInputSequence.load(std::memory_order_acquire) != 0 && pendingKind != 0
      && detail::inputPriority(detail::inputKind(pendingKind)) > detail::inputPriority(kind)) {
    NOCTALIA_TRACE_PLOT("Latency lower-priority inputs coalesced", static_cast<std::int64_t>(1));
    return;
  }
  const std::uint64_t sequence = state.nextSequence.fetch_add(1, std::memory_order_relaxed) + 1;
  state.inputForwardedNs.store(0, std::memory_order_release);
  state.firstPumpRequestNs.store(0, std::memory_order_release);
  state.firstAcceptedPumpRequestNs.store(0, std::memory_order_release);
  state.firstPumpDispatchNs.store(0, std::memory_order_release);
  state.latestPumpDispatchNs.store(0, std::memory_order_release);
  state.pumpDispatchesSinceInput.store(0, std::memory_order_release);
  state.immediateExternalBeginFrameNs.store(0, std::memory_order_release);
  state.latestExternalBeginFrameNs.store(0, std::memory_order_release);
  state.externalBeginFramesSinceInput.store(0, std::memory_order_release);
  state.inputReceivedNs.store(detail::nowNs(), std::memory_order_release);
  state.pendingInputKind.store(static_cast<std::int64_t>(kind), std::memory_order_release);
  const std::uint64_t previous = state.pendingInputSequence.exchange(sequence, std::memory_order_acq_rel);
  if (previous != 0) {
    const std::uint64_t count = state.coalescedInputs.fetch_add(1, std::memory_order_relaxed) + 1;
    NOCTALIA_TRACE_PLOT("Latency coalesced inputs", static_cast<std::int64_t>(count));
  }
  NOCTALIA_TRACE_PLOT("Latency input received seq", static_cast<std::int64_t>(sequence));
  NOCTALIA_TRACE_PLOT("Latency input kind", static_cast<std::int64_t>(kind));
}

inline void inputForwardedToCef(InputKind kind) {
  auto& state = detail::state();
  const std::uint64_t sequence = state.pendingInputSequence.load(std::memory_order_acquire);
  if (sequence == 0
      || state.pendingInputKind.load(std::memory_order_acquire) != static_cast<std::int64_t>(kind)) {
    return;
  }
  const std::int64_t now = detail::nowNs();
  state.inputForwardedNs.store(now, std::memory_order_release);
  NOCTALIA_TRACE_PLOT("Latency CEF input forwarded seq", static_cast<std::int64_t>(sequence));
  NOCTALIA_TRACE_PLOT(
      "Latency input to CEF forward ms",
      detail::elapsedMs(state.inputReceivedNs.load(std::memory_order_acquire), now)
  );
}

inline void messagePumpScheduled(std::int64_t delayMs, bool deadlineUpdated) {
  auto& state = detail::state();
  const std::uint64_t sequence = state.pendingInputSequence.load(std::memory_order_acquire);
  if (sequence == 0) {
    return;
  }
  const std::int64_t now = detail::nowNs();
  std::int64_t unset = 0;
  const bool firstRequest = state.firstPumpRequestNs.compare_exchange_strong(
      unset, now, std::memory_order_acq_rel
  );
  if (deadlineUpdated) {
    unset = 0;
    state.firstAcceptedPumpRequestNs.compare_exchange_strong(unset, now, std::memory_order_acq_rel);
  }
  NOCTALIA_TRACE_PLOT("Latency CEF pump scheduled seq", static_cast<std::int64_t>(sequence));
  NOCTALIA_TRACE_PLOT("Latency CEF pump requested delay ms", delayMs);
  NOCTALIA_TRACE_PLOT(
      "Latency CEF pump deadline updated", static_cast<std::int64_t>(deadlineUpdated ? 1 : 0)
  );
  if (firstRequest) {
    NOCTALIA_TRACE_PLOT(
        "Latency CEF forward to first pump request ms",
        detail::elapsedMs(state.inputForwardedNs.load(std::memory_order_acquire), now)
    );
  }
}

inline void messagePumpDispatched(std::int64_t deadlineLatenessMs) {
  auto& state = detail::state();
  const std::uint64_t sequence = state.pendingInputSequence.load(std::memory_order_acquire);
  if (sequence == 0) {
    return;
  }
  const std::int64_t now = detail::nowNs();
  state.latestPumpDispatchNs.store(now, std::memory_order_release);
  const std::uint64_t dispatchCount =
      state.pumpDispatchesSinceInput.fetch_add(1, std::memory_order_acq_rel) + 1;
  std::int64_t unset = 0;
  const bool firstDispatch = state.firstPumpDispatchNs.compare_exchange_strong(
      unset, now, std::memory_order_acq_rel
  );
  NOCTALIA_TRACE_PLOT("Latency CEF pump dispatched seq", static_cast<std::int64_t>(sequence));
  NOCTALIA_TRACE_PLOT("Latency CEF pump deadline lateness ms", deadlineLatenessMs);
  NOCTALIA_TRACE_PLOT("Latency CEF pump dispatches since input", static_cast<std::int64_t>(dispatchCount));
  if (firstDispatch) {
    NOCTALIA_TRACE_PLOT(
        "Latency input to first pump dispatch ms",
        detail::elapsedMs(state.inputReceivedNs.load(std::memory_order_acquire), now)
    );
    NOCTALIA_TRACE_PLOT(
        "Latency CEF forward to first pump dispatch ms",
        detail::elapsedMs(state.inputForwardedNs.load(std::memory_order_acquire), now)
    );
    const std::int64_t firstRequestNs = state.firstPumpRequestNs.load(std::memory_order_acquire);
    if (firstRequestNs > 0) {
      NOCTALIA_TRACE_PLOT(
          "Latency first pump request to dispatch ms",
          detail::elapsedMs(firstRequestNs, now)
      );
    }
    const std::int64_t firstAcceptedRequestNs =
        state.firstAcceptedPumpRequestNs.load(std::memory_order_acquire);
    if (firstAcceptedRequestNs > 0) {
      NOCTALIA_TRACE_PLOT(
          "Latency first accepted pump request to dispatch ms",
          detail::elapsedMs(firstAcceptedRequestNs, now)
      );
    }
  }
}

inline void externalBeginFrameIssued(bool immediate) {
  auto& state = detail::state();
  const std::uint64_t sequence = state.pendingInputSequence.load(std::memory_order_acquire);
  if (sequence == 0) {
    return;
  }
  const std::int64_t now = detail::nowNs();
  state.latestExternalBeginFrameNs.store(now, std::memory_order_release);
  const std::uint64_t count =
      state.externalBeginFramesSinceInput.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (immediate) {
    state.immediateExternalBeginFrameNs.store(now, std::memory_order_release);
  }
  NOCTALIA_TRACE_PLOT("Latency external begin frame seq", static_cast<std::int64_t>(sequence));
  NOCTALIA_TRACE_PLOT("Latency external begin frame immediate", static_cast<std::int64_t>(immediate));
  NOCTALIA_TRACE_PLOT("Latency external begin frames since input", static_cast<std::int64_t>(count));
  NOCTALIA_TRACE_PLOT(
      "Latency input to external begin frame ms",
      detail::elapsedMs(state.inputReceivedNs.load(std::memory_order_acquire), now)
  );
}

inline void externalBeginFrameCoalesced() {
  NOCTALIA_TRACE_PLOT("Latency coalesced external begin frames", static_cast<std::int64_t>(1));
}

inline void acceleratedPaintArrived() {
  auto& state = detail::state();
  const std::uint64_t sequence = state.pendingInputSequence.exchange(0, std::memory_order_acq_rel);
  if (sequence == 0) {
    return;
  }
  if (state.presentationSequence.load(std::memory_order_acquire) != 0) {
    const std::uint64_t count = state.supersededPaints.fetch_add(1, std::memory_order_relaxed) + 1;
    NOCTALIA_TRACE_PLOT("Latency superseded correlated paints", static_cast<std::int64_t>(count));
  }
  const std::int64_t now = detail::nowNs();
  const std::int64_t kind = state.pendingInputKind.exchange(0, std::memory_order_acq_rel);
  const std::int64_t inputNs = state.inputReceivedNs.load(std::memory_order_acquire);
  const std::int64_t forwardNs = state.inputForwardedNs.load(std::memory_order_acquire);
  const std::int64_t firstDispatchNs = state.firstPumpDispatchNs.load(std::memory_order_acquire);
  const std::int64_t latestDispatchNs = state.latestPumpDispatchNs.load(std::memory_order_acquire);
  const std::int64_t immediateBeginFrameNs =
      state.immediateExternalBeginFrameNs.load(std::memory_order_acquire);
  const std::int64_t latestBeginFrameNs =
      state.latestExternalBeginFrameNs.load(std::memory_order_acquire);
  state.presentationInputKind.store(kind, std::memory_order_release);
  state.presentationInputNs.store(inputNs, std::memory_order_release);
  state.acceleratedPaintNs.store(now, std::memory_order_release);
  state.redrawQueuedNs.store(0, std::memory_order_release);
  state.presentationSequence.store(sequence, std::memory_order_release);
  NOCTALIA_TRACE_PLOT("Latency accelerated paint seq", static_cast<std::int64_t>(sequence));
  NOCTALIA_TRACE_PLOT("Latency accelerated paint input kind", kind);
  const double inputToPaint = detail::elapsedMs(inputNs, now);
  const double forwardToPaint = detail::elapsedMs(forwardNs, now);
  const double firstDispatchToPaint = detail::elapsedMs(firstDispatchNs, now);
  NOCTALIA_TRACE_PLOT("Latency input to accelerated paint ms", inputToPaint);
  NOCTALIA_TRACE_PLOT("Latency CEF forward to accelerated paint ms", forwardToPaint);
  if (firstDispatchNs > 0) {
    NOCTALIA_TRACE_PLOT(
        "Latency first CEF pump dispatch to accelerated paint ms", firstDispatchToPaint
    );
    detail::plotFirstDispatchToPaint(detail::inputKind(kind), firstDispatchToPaint);
  }
  if (latestDispatchNs > 0) {
    NOCTALIA_TRACE_PLOT(
        "Latency latest CEF pump dispatch to accelerated paint ms",
        detail::elapsedMs(latestDispatchNs, now)
    );
  }
  if (immediateBeginFrameNs > 0) {
    const double immediateBeginToPaint = detail::elapsedMs(immediateBeginFrameNs, now);
    NOCTALIA_TRACE_PLOT(
        "Latency immediate external begin frame to accelerated paint ms", immediateBeginToPaint
    );
    detail::plotImmediateBeginFrameToPaint(detail::inputKind(kind), immediateBeginToPaint);
  }
  if (latestBeginFrameNs > 0) {
    NOCTALIA_TRACE_PLOT(
        "Latency latest external begin frame to accelerated paint ms",
        detail::elapsedMs(latestBeginFrameNs, now)
    );
  }
  const auto pumpDispatchCount = static_cast<std::int64_t>(
      state.pumpDispatchesSinceInput.load(std::memory_order_acquire)
  );
  NOCTALIA_TRACE_PLOT("Latency CEF pump dispatch count before accelerated paint", pumpDispatchCount);
  detail::plotInputToPaint(detail::inputKind(kind), inputToPaint);
  detail::plotForwardToPaint(detail::inputKind(kind), forwardToPaint);
  detail::plotPumpDispatchCount(detail::inputKind(kind), pumpDispatchCount);
}

inline void acceleratedPaintFailed() {
  auto& state = detail::state();
  state.pendingInputKind.store(0, std::memory_order_release);
  state.presentationSequence.store(0, std::memory_order_release);
  state.presentationInputKind.store(0, std::memory_order_release);
  state.presentationInputNs.store(0, std::memory_order_release);
  state.acceleratedPaintNs.store(0, std::memory_order_release);
  state.redrawQueuedNs.store(0, std::memory_order_release);
}

inline void redrawQueued() {
  auto& state = detail::state();
  const std::uint64_t sequence = state.presentationSequence.load(std::memory_order_acquire);
  if (sequence == 0) {
    return;
  }
  const std::int64_t now = detail::nowNs();
  state.redrawQueuedNs.store(now, std::memory_order_release);
  NOCTALIA_TRACE_PLOT("Latency redraw queued seq", static_cast<std::int64_t>(sequence));
  NOCTALIA_TRACE_PLOT(
      "Latency paint to redraw queue ms",
      detail::elapsedMs(state.acceleratedPaintNs.load(std::memory_order_acquire), now)
  );
}

inline void graphiteFrameBegan() {
  const auto& state = detail::state();
  const std::uint64_t sequence = state.presentationSequence.load(std::memory_order_acquire);
  if (sequence == 0) {
    return;
  }
  const std::int64_t now = detail::nowNs();
  NOCTALIA_TRACE_PLOT("Latency Graphite begin seq", static_cast<std::int64_t>(sequence));
  NOCTALIA_TRACE_PLOT(
      "Latency redraw queue to Graphite begin ms",
      detail::elapsedMs(state.redrawQueuedNs.load(std::memory_order_acquire), now)
  );
}

inline PresentationTrace presentationSubmitted() {
  auto& state = detail::state();
  const std::uint64_t sequence = state.presentationSequence.exchange(0, std::memory_order_acq_rel);
  if (sequence == 0) {
    return {};
  }
  const std::int64_t now = detail::nowNs();
  const std::int64_t kind = state.presentationInputKind.exchange(0, std::memory_order_acq_rel);
  const std::int64_t inputNs = state.presentationInputNs.exchange(0, std::memory_order_acq_rel);
  const std::int64_t paintNs = state.acceleratedPaintNs.exchange(0, std::memory_order_acq_rel);
  const std::int64_t redrawNs = state.redrawQueuedNs.exchange(0, std::memory_order_acq_rel);
  NOCTALIA_TRACE_PLOT("Latency Graphite presented seq", static_cast<std::int64_t>(sequence));
  NOCTALIA_TRACE_PLOT("Latency presented input kind", kind);
  const double inputToPresent = detail::elapsedMs(inputNs, now);
  const double paintToPresent = detail::elapsedMs(paintNs, now);
  NOCTALIA_TRACE_PLOT("Latency input to present ms", inputToPresent);
  NOCTALIA_TRACE_PLOT("Latency accelerated paint to present ms", paintToPresent);
  NOCTALIA_TRACE_PLOT("Latency redraw queue to present ms", detail::elapsedMs(redrawNs, now));
  detail::plotInputToPresent(detail::inputKind(kind), inputToPresent);
  detail::plotPaintToPresent(detail::inputKind(kind), paintToPresent);
  return {
      .sequence = sequence,
      .kind = kind,
      .inputNs = inputNs,
      .paintNs = paintNs,
      .redrawNs = redrawNs,
      .queuePresentNs = now,
  };
}

inline void compositorPresented(
    const PresentationTrace& trace, std::int64_t presentedSteadyNs,
    std::uint32_t refreshNs, std::int64_t feedbackDeliveryNs
) {
  NOCTALIA_TRACE_PLOT("Wayland presentation refresh ns", static_cast<std::int64_t>(refreshNs));
  NOCTALIA_TRACE_PLOT(
      "Wayland presentation feedback delivery ms",
      static_cast<double>(feedbackDeliveryNs) / 1'000'000.0
  );
  if (!trace.valid()) {
    return;
  }
  const double inputToVisible = detail::elapsedMs(trace.inputNs, presentedSteadyNs);
  NOCTALIA_TRACE_PLOT("Latency compositor presented seq", static_cast<std::int64_t>(trace.sequence));
  NOCTALIA_TRACE_PLOT("Latency input to visible ms", inputToVisible);
  NOCTALIA_TRACE_PLOT(
      "Latency queue present to visible ms",
      detail::elapsedMs(trace.queuePresentNs, presentedSteadyNs)
  );
  NOCTALIA_TRACE_PLOT(
      "Latency accelerated paint to visible ms",
      detail::elapsedMs(trace.paintNs, presentedSteadyNs)
  );
  detail::plotInputToVisible(detail::inputKind(trace.kind), inputToVisible);
}

inline void compositorDiscarded(const PresentationTrace& trace) {
  if (trace.valid()) {
    NOCTALIA_TRACE_PLOT("Latency compositor discarded seq", static_cast<std::int64_t>(trace.sequence));
  }
}

#else

inline void inputReceived(InputKind) {}
inline void inputForwardedToCef(InputKind) {}
inline void messagePumpScheduled(std::int64_t, bool) {}
inline void messagePumpDispatched(std::int64_t) {}
inline void externalBeginFrameIssued(bool) {}
inline void externalBeginFrameCoalesced() {}
inline void acceleratedPaintArrived() {}
inline void acceleratedPaintFailed() {}
inline void redrawQueued() {}
inline void graphiteFrameBegan() {}
inline PresentationTrace presentationSubmitted() { return {}; }
inline void compositorPresented(const PresentationTrace&, std::int64_t, std::uint32_t, std::int64_t) {}
inline void compositorDiscarded(const PresentationTrace&) {}

#endif

} // namespace tracy_latency
