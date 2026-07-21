#include "shell/apple_music/apple_music_panel.h"

#include "cef/cef_browser_session.h"
#include "ui/style.h"

AppleMusicPanel::AppleMusicPanel(std::shared_ptr<CefBrowserSession> session)
    : WebPanel(std::move(session), WebPanelSite::AppleMusic) {}

void AppleMusicPanel::setFullscreenPresentation(bool fullscreen) noexcept {
  m_fullscreen = fullscreen;
}

void AppleMusicPanel::preparePresentationResize(std::function<void()> ready) {
  m_session->preparePresentationResize(std::move(ready));
}

float AppleMusicPanel::webCornerRadius() const {
  return m_fullscreen ? 0.0f : Style::scaledRadiusXl(contentScale());
}
