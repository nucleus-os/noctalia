#pragma once

#include <string_view>

namespace internal_toplevel {

  inline constexpr std::string_view kAppleMusicFullscreenAppId = "dev.noctalia.AppleMusicFullscreen";

  [[nodiscard]] inline bool hiddenFromShellAppModel(std::string_view appId) noexcept {
    return appId == kAppleMusicFullscreenAppId;
  }

} // namespace internal_toplevel
