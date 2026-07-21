#include "shell/web_panel/web_panel_profile.h"

#include <array>
#include <cstddef>

namespace {
  constexpr std::array<WebPanelProfile, 2> kProfiles = {{
      {
          .site = WebPanelSite::AppleMusic,
          .panelId = "apple-music",
          .displayName = "Apple Music",
          .startUrl = "https://music.apple.com/",
          .preferredWidth = 1120.0f,
          .preferredHeight = 720.0f,
      },
      {
          .site = WebPanelSite::Discord,
          .panelId = "discord",
          .displayName = "Discord",
          .startUrl = "https://discord.com/app",
          .preferredWidth = 1180.0f,
          .preferredHeight = 760.0f,
      },
  }};
}

const WebPanelProfile& webPanelProfile(WebPanelSite site) {
  return kProfiles[static_cast<std::size_t>(site)];
}
