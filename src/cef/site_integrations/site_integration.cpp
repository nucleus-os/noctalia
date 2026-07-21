#include "cef/site_integrations/site_integration.h"

#include "cef/site_integrations/apple_music_integration.h"
#include "cef/site_integrations/discord_integration.h"
#include "cef/site_integrations/site_integration_policy.h"

#include <string>

void installWebPanelSiteIntegration(CefRefPtr<CefFrame> frame) {
  if (frame == nullptr || !frame->IsMain()) {
    return;
  }
  const std::string url = frame->GetURL().ToString();
  const SiteIntegration integration = siteIntegrationForUrl(url);
  if (integration == SiteIntegration::AppleMusic) {
    installAppleMusicSiteIntegration(frame);
  } else if (integration == SiteIntegration::Discord) {
    installDiscordSiteIntegration(frame);
  }
}
