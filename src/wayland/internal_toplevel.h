#pragma once

#include <string_view>

namespace internal_toplevel {

  // Shared app-id prefix for xdg_toplevels PanelManager::CefPanelToplevelHost creates on behalf
  // of a toplevel-presented panel (see Panel::usesToplevelPresentation()). Each open appends a
  // random nonce (e.g. "dev.noctalia.panel-open-apple-music-3f9a1c2b") so concurrently open CEF
  // panels never collide when niri reports which window just mapped.
  inline constexpr std::string_view kCefPanelAppIdPrefix = "dev.noctalia.panel-open-";

  [[nodiscard]] inline bool hiddenFromShellAppModel(std::string_view appId) noexcept {
    return appId.starts_with(kCefPanelAppIdPrefix);
  }

} // namespace internal_toplevel
