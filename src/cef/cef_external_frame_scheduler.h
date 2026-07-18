#pragma once

#include "render/presentation_timing.h"

#include <algorithm>
#include <cstdint>
#include <optional>

// Owns the client side of CEF's acknowledged external-BeginFrame contract.
// Paint delivery is deliberately absent from this state machine: only the
// matching Chromium BeginFrameAck completes an in-flight request.
class CefExternalFrameScheduler {
public:
  enum class State : std::uint8_t {
    Suspended,
    Idle,
    InFlight,
    Draining,
  };

  struct Request {
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    std::int64_t deadlineDeltaNs = 0;
    std::int64_t intervalNs = 0;
    bool urgent = false;
  };

  explicit CefExternalFrameScheduler(std::int64_t fallbackIntervalNs)
      : m_fallbackIntervalNs(std::max<std::int64_t>(1, fallbackIntervalNs)) {}

  void resume() noexcept {
    if (m_state == State::Draining) {
      m_resumeAfterDrain = true;
      return;
    }
    ++m_generation;
    m_state = State::Idle;
    m_pending = {};
    m_inFlightId = 0;
    // wp_presentation sequence values are scoped to an output's refresh
    // timeline. A reattached surface may be presented on another output, so
    // keep the last known interval as a useful fallback but relearn its phase
    // and sequence from the new surface's feedback.
    m_lastPresentedNs = 0;
    m_lastPresentationSequence = 0;
  }

  [[nodiscard]] bool suspend() noexcept {
    m_pending = {};
    m_resumeAfterDrain = false;
    m_lastPresentedNs = 0;
    m_lastPresentationSequence = 0;
    if (m_state == State::InFlight) {
      m_state = State::Draining;
      return true;
    }
    forceSuspend();
    return false;
  }

  void forceSuspend() noexcept {
    ++m_generation;
    m_state = State::Suspended;
    m_pending = {};
    m_inFlightId = 0;
    m_resumeAfterDrain = false;
    m_lastPresentedNs = 0;
    m_lastPresentationSequence = 0;
  }

  void onPresentation(const SurfacePresentationFeedback& feedback) noexcept {
    if (!feedback.presented || feedback.presentedSteadyNs <= 0) {
      return;
    }
    if (m_lastPresentationSequence != 0 && feedback.sequence < m_lastPresentationSequence) {
      return;
    }
    m_lastPresentationSequence = feedback.sequence;
    m_lastPresentedNs = feedback.presentedSteadyNs;
    if (feedback.refreshNs > 0) {
      m_refreshIntervalNs = feedback.refreshNs;
    }
  }

  // Records a compositor-paced opportunity. If Chromium is busy, only the
  // newest opportunity survives, while urgency is sticky until serviced.
  [[nodiscard]] std::optional<Request> onFrameOpportunity(std::int64_t nowNs) noexcept {
    return request(nowNs, false);
  }

  // Input, navigation, resize and explicit invalidation may request work
  // immediately instead of waiting for the next Wayland callback.
  [[nodiscard]] std::optional<Request> requestUrgent(std::int64_t nowNs) noexcept {
    return request(nowNs, true);
  }

  [[nodiscard]] std::optional<Request> requestNormal(std::int64_t nowNs) noexcept {
    return request(nowNs, false);
  }

  [[nodiscard]] std::optional<Request> acknowledge(
      std::uint64_t requestId, bool /*hasDamage*/, std::int64_t nowNs
  ) noexcept {
    if ((m_state != State::InFlight && m_state != State::Draining) || requestId != m_inFlightId) {
      return std::nullopt;
    }

    if (m_state == State::Draining) {
      const bool resumeAfterDrain = m_resumeAfterDrain;
      if (resumeAfterDrain) {
        m_state = State::Suspended;
        resume();
      } else {
        forceSuspend();
      }
      return std::nullopt;
    }

    m_state = State::Idle;
    m_inFlightId = 0;

    if (!m_pending.valid) {
      return std::nullopt;
    }
    const bool urgent = m_pending.urgent;
    m_pending = {};
    return begin(nowNs, urgent);
  }

  // Used only if CEF rejects a request synchronously. Once CEF accepts a
  // request, only its acknowledgment or a lifecycle generation change may
  // release the local in-flight state.
  bool abandon(std::uint64_t requestId) noexcept {
    if (m_state != State::InFlight || requestId != m_inFlightId) {
      return false;
    }
    m_state = State::Idle;
    m_inFlightId = 0;
    m_pending = {};
    return true;
  }

  [[nodiscard]] State state() const noexcept { return m_state; }
  [[nodiscard]] bool isInFlight(std::uint64_t requestId) const noexcept {
    return (m_state == State::InFlight || m_state == State::Draining) && m_inFlightId == requestId;
  }
  [[nodiscard]] bool needsFrameOpportunity() const noexcept {
    return m_state == State::Idle || m_state == State::InFlight;
  }
  [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }
  [[nodiscard]] std::int64_t intervalNs() const noexcept {
    return m_refreshIntervalNs > 0 ? m_refreshIntervalNs : m_fallbackIntervalNs;
  }

private:
  struct PendingOpportunity {
    bool valid = false;
    bool urgent = false;
  };

  [[nodiscard]] std::optional<Request> request(std::int64_t nowNs, bool urgent) noexcept {
    if (m_state == State::Suspended || m_state == State::Draining) {
      return std::nullopt;
    }
    if (m_state == State::InFlight) {
      m_pending.valid = true;
      m_pending.urgent = m_pending.urgent || urgent;
      return std::nullopt;
    }

    urgent = urgent || m_pending.urgent;
    m_pending = {};
    return begin(nowNs, urgent);
  }

  [[nodiscard]] Request begin(std::int64_t nowNs, bool urgent) noexcept {
    const std::int64_t interval = intervalNs();
    std::int64_t targetNs = nowNs + interval;
    if (m_lastPresentedNs > 0) {
      targetNs = m_lastPresentedNs + interval;
      if (targetNs <= nowNs) {
        const std::int64_t elapsed = nowNs - targetNs;
        targetNs += (elapsed / interval + 1) * interval;
      }
    }

    m_state = State::InFlight;
    m_inFlightId = ++m_nextRequestId;
    return Request{
        .id = m_inFlightId,
        .generation = m_generation,
        .deadlineDeltaNs = std::max<std::int64_t>(1, targetNs - nowNs),
        .intervalNs = interval,
        .urgent = urgent,
    };
  }

  State m_state = State::Suspended;
  PendingOpportunity m_pending;
  std::int64_t m_fallbackIntervalNs = 1;
  std::int64_t m_refreshIntervalNs = 0;
  std::int64_t m_lastPresentedNs = 0;
  std::uint64_t m_lastPresentationSequence = 0;
  std::uint64_t m_generation = 0;
  std::uint64_t m_nextRequestId = 0;
  std::uint64_t m_inFlightId = 0;
  bool m_resumeAfterDrain = false;
};
