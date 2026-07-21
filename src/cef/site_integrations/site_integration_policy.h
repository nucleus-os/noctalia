#pragma once

#include <string_view>

enum class SiteIntegration {
  None,
  AppleMusic,
  Discord,
};

[[nodiscard]] SiteIntegration siteIntegrationForUrl(std::string_view url) noexcept;
[[nodiscard]] bool isTrustedUrlForCefSession(std::string_view sessionId, std::string_view url) noexcept;
[[nodiscard]] bool isAllowedTopLevelUrlForCefSession(
    std::string_view sessionId, std::string_view url, bool auxiliary
) noexcept;
