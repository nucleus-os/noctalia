#pragma once

#include <cstdint>

// Tracks the browser-side renderer recovery boundary without depending on CEF
// types. One reload is allowed for a termination. A second termination before
// a new render view becomes ready is treated as a failed recovery rather than
// entering an unbounded crash/reload loop.
class CefRendererRecovery {
public:
  enum class State : std::uint8_t {
    Ready,
    WaitingForRenderView,
    Failed,
  };

  enum class Action : std::uint8_t {
    Reload,
    Stop,
  };

  [[nodiscard]] Action onTerminated() noexcept {
    ++m_generation;
    if (m_state != State::Ready) {
      m_state = State::Failed;
      return Action::Stop;
    }
    m_state = State::WaitingForRenderView;
    return Action::Reload;
  }

  void onRenderViewReady() noexcept {
    m_state = State::Ready;
  }

  void reset() noexcept {
    ++m_generation;
    m_state = State::Ready;
  }

  [[nodiscard]] State state() const noexcept { return m_state; }
  [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }
  [[nodiscard]] bool isPending(std::uint64_t generation) const noexcept {
    return m_state == State::WaitingForRenderView && m_generation == generation;
  }

private:
  State m_state = State::Ready;
  std::uint64_t m_generation = 0;
};
