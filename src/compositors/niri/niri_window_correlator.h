#pragma once

#include "compositors/niri/niri_event_handler.h"

#include <cstdint>
#include <functional>
#include <string>

namespace compositors::niri {

  class NiriRuntime;

  // Watches the niri event stream for the WindowOpenedOrChanged event whose window matches a
  // caller-chosen (app_id, pid) pair, then reports the window's niri-internal id once.
  //
  // Replaces polling `"Windows"` on a timer (as the old AppleMusicFullscreenHost did) with a
  // push-based match against the already-connected event stream: the caller opens an
  // xdg_toplevel with a nonce'd app_id (so concurrent opens can't collide), constructs one of
  // these to watch for it, and gets called back as soon as niri reports the window mapped.
  class NiriWindowCorrelator : public NiriEventHandler {
  public:
    NiriWindowCorrelator(NiriRuntime& runtime, std::string expectedAppId, long expectedPid,
                          std::function<void(std::uint64_t windowId)> onMatched);

    void handleEvent(std::string_view key, const nlohmann::json& value) override;

    [[nodiscard]] bool matched() const noexcept { return m_matched; }

  private:
    std::string m_expectedAppId;
    long m_expectedPid;
    std::function<void(std::uint64_t windowId)> m_onMatched;
    bool m_matched = false;
  };

} // namespace compositors::niri
