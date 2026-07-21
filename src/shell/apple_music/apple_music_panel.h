#pragma once

#include "shell/web_panel/web_panel.h"

class CefBrowserSession;
class AppleMusicPanel final : public WebPanel {
public:
  explicit AppleMusicPanel(std::shared_ptr<CefBrowserSession> session);
  [[nodiscard]] bool supportsFullscreenPresentation() const noexcept override { return true; }
  [[nodiscard]] bool fullscreenPresentation() const noexcept override { return m_fullscreen; }
  void setFullscreenPresentation(bool fullscreen) noexcept override;
  void preparePresentationResize(std::function<void()> ready) override;
  [[nodiscard]] bool hasDecoration() const override { return !m_fullscreen; }
  [[nodiscard]] std::string panelScreenPosition() const override { return "center"; }

protected:
  [[nodiscard]] float webCornerRadius() const override;

private:
  bool m_fullscreen = false;
};
