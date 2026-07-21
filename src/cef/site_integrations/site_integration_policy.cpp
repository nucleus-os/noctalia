#include "cef/site_integrations/site_integration_policy.h"

namespace {
  bool hasHttpsOrigin(std::string_view url, std::string_view host) noexcept {
    constexpr std::string_view kScheme = "https://";
    if (!url.starts_with(kScheme)) {
      return false;
    }
    url.remove_prefix(kScheme.size());
    if (!url.starts_with(host)) {
      return false;
    }
    if (url.size() == host.size()) {
      return true;
    }
    const char delimiter = url[host.size()];
    return delimiter == '/' || delimiter == '?' || delimiter == '#';
  }
}

SiteIntegration siteIntegrationForUrl(std::string_view url) noexcept {
  if (hasHttpsOrigin(url, "music.apple.com")) {
    return SiteIntegration::AppleMusic;
  }
  if (hasHttpsOrigin(url, "discord.com")) {
    return SiteIntegration::Discord;
  }
  return SiteIntegration::None;
}

bool isTrustedUrlForCefSession(std::string_view sessionId, std::string_view url) noexcept {
  const SiteIntegration integration = siteIntegrationForUrl(url);
  return (sessionId == "apple-music" && integration == SiteIntegration::AppleMusic)
      || (sessionId == "discord" && integration == SiteIntegration::Discord);
}

bool isAllowedTopLevelUrlForCefSession(
    std::string_view sessionId, std::string_view url, bool auxiliary
) noexcept {
  return isTrustedUrlForCefSession(sessionId, url)
      || (auxiliary && (url.empty() || url == "about:blank"));
}
