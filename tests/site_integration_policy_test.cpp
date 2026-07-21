#include "cef/site_integrations/site_integration_policy.h"

#include <cassert>

int main() {
  assert(siteIntegrationForUrl("https://music.apple.com/") == SiteIntegration::AppleMusic);
  assert(siteIntegrationForUrl("https://music.apple.com/us/home") == SiteIntegration::AppleMusic);
  assert(siteIntegrationForUrl("https://discord.com") == SiteIntegration::Discord);
  assert(siteIntegrationForUrl("https://discord.com?redirect=/channels/@me") == SiteIntegration::Discord);
  assert(siteIntegrationForUrl("https://discord.com#login") == SiteIntegration::Discord);
  assert(siteIntegrationForUrl("https://discord.com/channels/@me") == SiteIntegration::Discord);
  assert(siteIntegrationForUrl("http://discord.com/") == SiteIntegration::None);
  assert(siteIntegrationForUrl("https://discord.com.evil.example/") == SiteIntegration::None);
  assert(siteIntegrationForUrl("https://music.apple.com.evil.example/") == SiteIntegration::None);
  assert(isTrustedUrlForCefSession("discord", "https://discord.com/login"));
  assert(!isTrustedUrlForCefSession("discord", "https://music.apple.com/"));
  assert(isTrustedUrlForCefSession("apple-music", "https://music.apple.com/us/home"));
  assert(isAllowedTopLevelUrlForCefSession("discord", "https://discord.com/channels/@me", false));
  assert(!isAllowedTopLevelUrlForCefSession("discord", "https://example.com/", false));
  assert(isAllowedTopLevelUrlForCefSession("discord", "about:blank", true));
  assert(!isAllowedTopLevelUrlForCefSession("discord", "about:blank", false));
  assert(!isAllowedTopLevelUrlForCefSession("discord", "https://example.com/", true));
  return 0;
}
