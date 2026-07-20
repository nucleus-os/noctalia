#pragma once

#include "shell/panel/panel.h"

class CefService;
class CefSurfaceNode;

class AppleMusicPanel final : public Panel {
public:
  explicit AppleMusicPanel(CefService& service);

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  void onFrameTick(float deltaMs) override;
  void onPresentation(const SurfacePresentationFeedback& feedback) override;

  [[nodiscard]] float preferredWidth() const override { return scaled(1120.0f); }
  [[nodiscard]] float preferredHeight() const override { return scaled(720.0f); }
  [[nodiscard]] bool supportsFullscreenPresentation() const noexcept override { return true; }
  [[nodiscard]] bool fullscreenPresentation() const noexcept override { return m_fullscreen; }
  void setFullscreenPresentation(bool fullscreen) noexcept override;
  void preparePresentationResize(std::function<void()> ready) override;
  void setPresentationTransfer(bool transferring) noexcept override { m_presentationTransfer = transferring; }
  [[nodiscard]] bool hasDecoration() const override { return !m_fullscreen; }
  [[nodiscard]] bool usesContentPadding() const noexcept override { return false; }
  [[nodiscard]] bool detachedBackgroundInheritsSourceBarOpacity() const noexcept override { return true; }
  [[nodiscard]] LayerShellLayer layer() const override { return LayerShellLayer::Overlay; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override { return PanelPlacement::Floating; }
  [[nodiscard]] std::string panelScreenPosition() const override { return "center"; }

protected:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;

private:
  CefService& m_service;
  CefSurfaceNode* m_surface = nullptr;
  bool m_initialNavigationRequested = false;
  bool m_fullscreen = false;
  bool m_presentationTransfer = false;
};
