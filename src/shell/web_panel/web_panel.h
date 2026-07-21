#pragma once

#include "cef/cef_browser_session.h"
#include "shell/panel/panel.h"
#include "shell/web_panel/web_panel_profile.h"

#include <memory>
#include <optional>
#include <vector>

class CefSurfaceNode;
class Button;
class Flex;
class Label;
class Spinner;
class ContextMenuPopup;

// Shared native host for a trusted, code-defined windowless web panel. Site
// DOM policy and exceptional product behavior stay outside this class.
class WebPanel : public Panel {
public:
  WebPanel(std::shared_ptr<CefBrowserSession> session, WebPanelSite site);
  ~WebPanel() override;

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  void onFrameTick(float deltaMs) override;
  void onPresentation(const SurfacePresentationFeedback& feedback) override;

  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  void setPresentationTransfer(bool transferring) noexcept override { m_presentationTransfer = transferring; }
  [[nodiscard]] bool usesContentPadding() const noexcept override { return false; }
  [[nodiscard]] bool detachedBackgroundInheritsSourceBarOpacity() const noexcept override { return true; }
  [[nodiscard]] LayerShellLayer layer() const override { return LayerShellLayer::Overlay; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override { return PanelPlacement::Floating; }
  [[nodiscard]] std::string panelScreenPosition() const override { return "auto"; }
  [[nodiscard]] bool panelOpenNearClick() const override { return true; }

  [[nodiscard]] const WebPanelProfile& profile() const noexcept { return m_profile; }
  [[nodiscard]] const std::shared_ptr<CefBrowserSession>& session() const noexcept { return m_session; }

protected:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  [[nodiscard]] virtual float webCornerRadius() const;
  void refreshSessionStateUi();
  void showPermissionRequest(CefBrowserPermissionRequest request);
  void showNextPermissionDecision();
  void resolvePermissionRequest(bool allowed);
  void cancelPermissionRequest();
  void showContextMenu(CefBrowserContextMenuRequest request);
  void closeContextMenu();
  void showBrowserPopup(CefBrowserPopupRequest request);
  void closeBrowserPopup(bool closeSession);
  void removeBrowserPopup(CefBrowserSession* session, bool closeSession);

  std::shared_ptr<CefBrowserSession> m_session;
  CefSurfaceNode* m_surface = nullptr;
  Flex* m_statusOverlay = nullptr;
  Spinner* m_statusSpinner = nullptr;
  Label* m_statusLabel = nullptr;
  Button* m_retryButton = nullptr;
  Flex* m_permissionOverlay = nullptr;
  Label* m_permissionLabel = nullptr;
  std::optional<CefBrowserPermissionRequest> m_permissionRequest;
  std::uint32_t m_permissionAllowed = CefBrowserPermissionNone;
  std::uint32_t m_permissionUndecided = CefBrowserPermissionNone;
  std::uint32_t m_permissionCurrent = CefBrowserPermissionNone;
  std::optional<CefBrowserContextMenuRequest> m_contextMenuRequest;
  std::unique_ptr<ContextMenuPopup> m_contextMenu;
  struct PopupPresentation {
    std::shared_ptr<CefBrowserSession> session;
    Node* layer = nullptr;
    CefSurfaceNode* surface = nullptr;
    Button* closeButton = nullptr;
    int preferredWidth = 0;
    int preferredHeight = 0;
  };
  std::vector<PopupPresentation> m_popupStack;
  bool m_presentationTransfer = false;

private:
  const WebPanelProfile& m_profile;
  bool m_initialNavigationRequested = false;
};
