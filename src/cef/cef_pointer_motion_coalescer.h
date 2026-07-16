#pragma once

#include <cstdint>
#include <optional>
#include <utility>

struct CefPointerMotion {
  int x = 0;
  int y = 0;
  std::uint32_t modifiers = 0;

  bool operator==(const CefPointerMotion&) const = default;
};

// Retains the newest pointer position until the browser's next frame
// opportunity. CEF consumes integer coordinates, so duplicate logical motion
// that truncates to the same pixel can be suppressed before reaching Blink.
class CefPointerMotionCoalescer {
public:
  bool queue(CefPointerMotion motion) {
    if ((m_pending && *m_pending == motion) || (!m_pending && m_lastSent && *m_lastSent == motion)) {
      return false;
    }
    m_pending = motion;
    return true;
  }

  [[nodiscard]] std::optional<CefPointerMotion> take() {
    if (!m_pending) {
      return std::nullopt;
    }
    m_lastSent = m_pending;
    return std::exchange(m_pending, std::nullopt);
  }

  void discardPending() { m_pending.reset(); }
  void reset() {
    m_pending.reset();
    m_lastSent.reset();
  }
  [[nodiscard]] bool pending() const noexcept { return m_pending.has_value(); }

private:
  std::optional<CefPointerMotion> m_pending;
  std::optional<CefPointerMotion> m_lastSent;
};
