#pragma once

#include <string_view>

enum class WebPanelSite {
  AppleMusic,
  Discord,
};

struct WebPanelProfile {
  WebPanelSite site;
  std::string_view panelId;
  std::string_view startUrl;
  float preferredWidth;
  float preferredHeight;
};

[[nodiscard]] const WebPanelProfile& webPanelProfile(WebPanelSite site);
