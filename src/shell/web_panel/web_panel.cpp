#include "shell/web_panel/web_panel.h"

#include "cef/cef_browser_session.h"
#include "cef/cef_surface_node.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/tracy.h"
#include "core/tracy_latency.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/spinner.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

#include <memory>
#include <utility>

WebPanel::WebPanel(std::shared_ptr<CefBrowserSession> session, WebPanelSite site, bool toplevelPresentation)
    : m_session(std::move(session)), m_profile(webPanelProfile(site)), m_toplevelPresentation(toplevelPresentation) {}

WebPanel::~WebPanel() = default;

namespace {
  constexpr std::string_view kPermissionStateOwner = "web_panel_permissions";
  constexpr std::size_t kMaxPresentedPopupDepth = 3;

  std::string permissionStateKey(std::string_view origin, std::string_view permission) {
    std::string key;
    key.reserve(origin.size() + permission.size() + 1);
    for (const char c : origin) {
      const unsigned char value = static_cast<unsigned char>(c);
      key.push_back(std::isalnum(value) != 0 ? static_cast<char>(std::tolower(value)) : '_');
    }
    key.push_back('_');
    key.append(permission);
    return key;
  }

  constexpr std::array<std::pair<std::uint32_t, std::string_view>, 4> kRememberedPermissions{{
      {CefBrowserPermissionMicrophone, "microphone"},
      {CefBrowserPermissionCamera, "camera"},
      {CefBrowserPermissionNotifications, "notifications"},
      {CefBrowserPermissionClipboard, "clipboard"},
  }};
} // namespace

void WebPanel::create() {
  auto root = std::make_unique<Node>();
  auto surface = std::make_unique<CefSurfaceNode>(*m_session);
  m_surface = surface.get();
  m_surface->attach(
      [this]() {
        NOCTALIA_TRACE_ZONE("Web panel CEF frame-ready request");
        tracy_latency::redrawQueued();
        if (auto* host = surfaceHost(); host != nullptr) {
          host->requestUpdateOnly();
          host->requestRedraw();
        }
      },
      [this]() {
        if (auto* host = surfaceHost(); host != nullptr) {
          host->requestCallbackTick();
        }
      },
      [this]() {
        if (auto* host = surfaceHost(); host != nullptr) {
          host->inputDispatcher().refreshCursor();
        }
      }
  );
  root->addChild(std::move(surface));

  auto overlay = ui::column(
      {
          .out = &m_statusOverlay,
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .gap = Style::spaceMd * contentScale(),
          .fillWidth = true,
          .fillHeight = true,
          .visible = false,
      },
      ui::spinner({
          .out = &m_statusSpinner,
          .spinnerSize = Style::fontSizeTitle * contentScale() * 1.5f,
          .spinning = true,
      }),
      ui::label({
          .out = &m_statusLabel,
          .text = i18n::tr("web-panel.loading"),
          .fontSize = Style::fontSizeBody * contentScale(),
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
          .textAlign = TextAlign::Center,
      }),
      ui::button({
          .out = &m_retryButton,
          .text = i18n::tr("web-panel.retry"),
          .variant = ButtonVariant::Primary,
          .visible = false,
          .onClick = [this]() { m_session->reload(); },
      })
  );
  overlay->setZIndex(1);
  root->addChild(std::move(overlay));

  auto permissionOverlay = ui::column(
      {
          .out = &m_permissionOverlay,
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .gap = Style::spaceLg * contentScale(),
          .padding = Style::spaceLg * contentScale() * 2.0f,
          .fill = colorSpecFromRole(ColorRole::Surface, 0.94f),
          .radius = webCornerRadius(),
          .fillWidth = true,
          .fillHeight = true,
          .visible = false,
      },
      ui::label({
          .out = &m_permissionLabel,
          .text = i18n::tr("web-panel.permission.microphone"),
          .fontSize = Style::fontSizeTitle * contentScale(),
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 3,
          .textAlign = TextAlign::Center,
      }),
      ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceMd * contentScale()},
          ui::button({
              .text = i18n::tr("common.actions.deny"),
              .variant = ButtonVariant::Outline,
              .onClick = [this]() { resolvePermissionRequest(false); },
          }),
          ui::button({
              .text = i18n::tr("common.actions.allow"),
              .variant = ButtonVariant::Primary,
              .onClick = [this]() { resolvePermissionRequest(true); },
          })
      )
  );
  permissionOverlay->setZIndex(2);
  root->addChild(std::move(permissionOverlay));

  m_session->setStateCallback([this](CefBrowserSessionState) {
    refreshSessionStateUi();
    if (auto* host = surfaceHost(); host != nullptr) {
      host->requestUpdateOnly();
      host->requestRedraw();
    }
  });
  m_session->setPermissionRequestCallback(
      [this](CefBrowserPermissionRequest request) { showPermissionRequest(std::move(request)); }
  );
  m_session->setContextMenuRequestCallback(
      [this](CefBrowserContextMenuRequest request) { showContextMenu(std::move(request)); }
  );
  m_session->setPopupCreatedCallback(
      [this](CefBrowserPopupRequest request) { showBrowserPopup(std::move(request)); }
  );
  refreshSessionStateUi();
  setRoot(std::move(root));
}

void WebPanel::onOpen(std::string_view /*context*/) {
  if (!m_initialNavigationRequested) {
    m_session->navigate(std::string(m_profile.startUrl));
    m_initialNavigationRequested = true;
  }
  m_session->setDisplayAttached(true);
}

void WebPanel::onClose() {
  m_session->setStateCallback(nullptr);
  m_session->setPermissionRequestCallback(nullptr);
  m_session->setContextMenuRequestCallback(nullptr);
  m_session->setPopupCreatedCallback(nullptr);
  closeContextMenu();
  while (!m_popupStack.empty()) {
    closeBrowserPopup(true);
  }
  cancelPermissionRequest();
  if (m_surface != nullptr) {
    m_surface->detach();
    m_surface = nullptr;
  }
  m_statusOverlay = nullptr;
  m_statusSpinner = nullptr;
  m_statusLabel = nullptr;
  m_retryButton = nullptr;
  m_permissionOverlay = nullptr;
  m_permissionLabel = nullptr;
}

void WebPanel::onFrameTick(float /*deltaMs*/) {
  bool needsAnother = m_session->onFrameOpportunity();
  if (!m_popupStack.empty()) {
    needsAnother = m_popupStack.back().session->onFrameOpportunity() || needsAnother;
  }
  if (needsAnother) {
    if (auto* host = surfaceHost(); host != nullptr) {
      host->requestCallbackTick();
    }
  }
}

void WebPanel::onPresentation(const SurfacePresentationFeedback& feedback) {
  m_session->onPresentation(feedback);
  if (!m_popupStack.empty()) {
    m_popupStack.back().session->onPresentation(feedback);
  }
}

float WebPanel::preferredWidth() const { return scaled(m_profile.preferredWidth); }

float WebPanel::preferredHeight() const { return scaled(m_profile.preferredHeight); }

InputArea* WebPanel::initialFocusArea() const { return m_surface != nullptr ? m_surface->inputArea() : nullptr; }

float WebPanel::webCornerRadius() const { return Style::scaledRadiusXl(contentScale()); }

void WebPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_surface == nullptr) {
    return;
  }
  m_surface->setPosition(0.0f, 0.0f);
  m_surface->setSize(width, height);
  m_surface->setCornerRadius(webCornerRadius());
  m_surface->layout(renderer);
  if (m_statusOverlay != nullptr) {
    m_statusOverlay->setPosition(0.0f, 0.0f);
    m_statusOverlay->setSize(width, height);
    m_statusOverlay->layout(renderer);
  }
  if (m_permissionOverlay != nullptr) {
    m_permissionOverlay->setPosition(0.0f, 0.0f);
    m_permissionOverlay->setSize(width, height);
    m_permissionOverlay->layout(renderer);
  }
  if (!m_popupStack.empty()) {
    auto& popup = m_popupStack.back();
    const float margin = scaled(Style::spaceLg);
    const float availableWidth = std::max(1.0f, width - margin * 2.0f);
    const float availableHeight = std::max(1.0f, height - margin * 2.0f);
    const float requestedWidth = popup.preferredWidth > 0 ? static_cast<float>(popup.preferredWidth) : width * 0.82f;
    const float requestedHeight =
        popup.preferredHeight > 0 ? static_cast<float>(popup.preferredHeight) : height * 0.82f;
    const float popupWidth = std::clamp(requestedWidth, std::min(420.0f, availableWidth), availableWidth);
    const float popupHeight = std::clamp(requestedHeight, std::min(320.0f, availableHeight), availableHeight);
    const float popupX = (width - popupWidth) * 0.5f;
    const float popupY = (height - popupHeight) * 0.5f;
    popup.layer->setPosition(popupX, popupY);
    popup.layer->setSize(popupWidth, popupHeight);
    popup.surface->setPosition(0.0f, 0.0f);
    popup.surface->setSize(popupWidth, popupHeight);
    popup.surface->setCornerRadius(webCornerRadius());
    popup.surface->layout(renderer);
    const float closeSize = scaled(40.0f);
    popup.closeButton->setPosition(popupWidth - closeSize - scaled(Style::spaceSm), scaled(Style::spaceSm));
    popup.closeButton->setSize(closeSize, closeSize);
    popup.closeButton->layout(renderer);
  }
}

void WebPanel::doUpdate(Renderer& renderer) {
  NOCTALIA_TRACE_ZONE("Web panel adopt CEF texture");
  if (m_surface != nullptr && m_surface->syncTexture(renderer.textureManager())) {
    m_surface->markPaintDirty();
  }
  if (!m_popupStack.empty()) {
    auto& popup = m_popupStack.back();
    if (popup.surface->syncTexture(renderer.textureManager())) {
      popup.surface->markPaintDirty();
    }
  }
}

void WebPanel::refreshSessionStateUi() {
  if (m_statusOverlay == nullptr || m_surface == nullptr) {
    return;
  }
  const bool show = !m_session->hasUsableFrame();
  m_statusOverlay->setVisible(show);
  m_surface->inputArea()->setEnabled(!show && !m_permissionRequest.has_value());
  if (!show) {
    if (m_statusSpinner != nullptr) {
      m_statusSpinner->stop();
    }
    return;
  }

  const CefBrowserSessionState state = m_session->state();
  const bool failed = state == CefBrowserSessionState::Failed || state == CefBrowserSessionState::Fatal;
  if (m_statusSpinner != nullptr) {
    if (failed) {
      m_statusSpinner->stop();
    } else {
      m_statusSpinner->start();
    }
    m_statusSpinner->setVisible(!failed);
  }
  if (m_retryButton != nullptr) {
    m_retryButton->setVisible(failed);
  }
  if (m_statusLabel != nullptr) {
    if (state == CefBrowserSessionState::Recovering) {
      m_statusLabel->setText(i18n::tr("web-panel.recovering"));
    } else if (failed) {
      m_statusLabel->setText(i18n::tr("web-panel.failed"));
    } else {
      m_statusLabel->setText(i18n::tr("web-panel.loading"));
    }
  }
}

void WebPanel::showPermissionRequest(CefBrowserPermissionRequest request) {
  cancelPermissionRequest();
  std::uint32_t allowed = CefBrowserPermissionNone;
  std::uint32_t undecided = CefBrowserPermissionNone;
  if (auto* config = PanelManager::instance().configService(); config != nullptr) {
    for (const auto& [permission, name] : kRememberedPermissions) {
      if ((request.permissions & permission) == 0) {
        continue;
      }
      const auto remembered = config->stateBool(kPermissionStateOwner, permissionStateKey(request.origin, name));
      if (!remembered.has_value()) {
        undecided |= permission;
      } else if (*remembered) {
        allowed |= permission;
      }
    }
  } else {
    undecided = request.permissions;
  }
  if (undecided == CefBrowserPermissionNone) {
    if (request.resolve) {
      request.resolve(allowed);
    }
    return;
  }
  m_permissionRequest = std::move(request);
  m_permissionAllowed = allowed;
  m_permissionUndecided = undecided;
  showNextPermissionDecision();
}

void WebPanel::showNextPermissionDecision() {
  m_permissionCurrent = CefBrowserPermissionNone;
  for (const auto& [permission, _] : kRememberedPermissions) {
    if ((m_permissionUndecided & permission) != 0) {
      m_permissionCurrent = permission;
      break;
    }
  }
  if (m_permissionCurrent == CefBrowserPermissionNone) {
    return;
  }
  if (m_permissionLabel != nullptr) {
    const char* key = m_permissionCurrent == CefBrowserPermissionNotifications
        ? "web-panel.permission.notifications"
        : m_permissionCurrent == CefBrowserPermissionClipboard ? "web-panel.permission.clipboard"
        : m_permissionCurrent == CefBrowserPermissionCamera    ? "web-panel.permission.camera"
                                                               : "web-panel.permission.microphone";
    m_permissionLabel->setText(i18n::tr(key));
  }
  if (m_permissionOverlay != nullptr) {
    m_permissionOverlay->setVisible(true);
  }
  if (m_surface != nullptr) {
    m_surface->inputArea()->setEnabled(false);
  }
  if (auto* host = surfaceHost(); host != nullptr) {
    host->requestUpdateOnly();
    host->requestRedraw();
  }
}

void WebPanel::resolvePermissionRequest(bool allowed) {
  if (!m_permissionRequest.has_value() || m_permissionCurrent == CefBrowserPermissionNone) {
    return;
  }
  if (auto* config = PanelManager::instance().configService(); config != nullptr) {
    for (const auto& [permission, name] : kRememberedPermissions) {
      if (permission == m_permissionCurrent) {
        (void)config->setStateBool(
            kPermissionStateOwner, permissionStateKey(m_permissionRequest->origin, name), allowed
        );
        break;
      }
    }
  }
  if (allowed) {
    m_permissionAllowed |= m_permissionCurrent;
  }
  m_permissionUndecided &= ~m_permissionCurrent;
  m_permissionCurrent = CefBrowserPermissionNone;
  if (m_permissionUndecided != CefBrowserPermissionNone) {
    showNextPermissionDecision();
    return;
  }
  auto request = std::move(*m_permissionRequest);
  m_permissionRequest.reset();
  if (m_permissionOverlay != nullptr) {
    m_permissionOverlay->setVisible(false);
  }
  if (m_surface != nullptr) {
    m_surface->inputArea()->setEnabled(m_session->hasUsableFrame());
  }
  if (request.resolve) {
    request.resolve(m_permissionAllowed);
  }
  m_permissionAllowed = CefBrowserPermissionNone;
}

void WebPanel::cancelPermissionRequest() {
  if (m_permissionRequest.has_value()) {
    auto request = std::move(*m_permissionRequest);
    m_permissionRequest.reset();
    if (request.resolve) {
      request.resolve(CefBrowserPermissionNone);
    }
  }
  m_permissionAllowed = CefBrowserPermissionNone;
  m_permissionUndecided = CefBrowserPermissionNone;
  m_permissionCurrent = CefBrowserPermissionNone;
  if (m_permissionOverlay != nullptr) {
    m_permissionOverlay->setVisible(false);
  }
}

void WebPanel::showContextMenu(CefBrowserContextMenuRequest request) {
  closeContextMenu();
  const int requestX = request.x;
  const int requestY = request.y;
  auto& manager = PanelManager::instance();
  auto* wayland = manager.wayland();
  auto* renderContext = manager.renderContext();
  // Prefer this panel's own host (its xdg_surface for toplevel-presented panels) over the
  // native-panel-only fallback, so context menus work regardless of whether any native panel
  // happens to be open at the same time.
  auto* host = surfaceHost();
  const auto parent = host != nullptr ? host->popupParentContext() : manager.fallbackPopupParentContext();
  if (wayland == nullptr || renderContext == nullptr || !parent.has_value()) {
    if (request.complete) {
      request.complete(std::nullopt);
    }
    return;
  }

  std::vector<ContextMenuControlEntry> entries;
  entries.reserve(request.entries.size());
  for (const auto& source : request.entries) {
    entries.push_back(ContextMenuControlEntry{
        .id = source.commandId,
        .label = source.label,
        .enabled = source.enabled,
        .separator = source.separator,
        .checkmark = source.checkmark,
        .radio = source.radio,
        .toggleState = source.checkmark || source.radio ? (source.checked ? 1 : 0) : -1,
    });
  }

  m_contextMenuRequest = std::move(request);
  m_contextMenu = std::make_unique<ContextMenuPopup>(*wayland, *renderContext);
  auto* popup = m_contextMenu.get();
  popup->setOnActivate([this](const ContextMenuControlEntry& entry) {
    if (!m_contextMenuRequest.has_value()) {
      return;
    }
    auto request = std::move(*m_contextMenuRequest);
    m_contextMenuRequest.reset();
    if (request.complete) {
      request.complete(entry.id);
    }
  });
  popup->setOnDismissed([this]() {
    PanelManager::instance().clearActivePopup();
    if (!m_contextMenuRequest.has_value()) {
      return;
    }
    auto request = std::move(*m_contextMenuRequest);
    m_contextMenuRequest.reset();
    if (request.complete) {
      request.complete(std::nullopt);
    }
  });
  manager.setActivePopup(popup);
  popup->open(ContextMenuPopupRequest{
      .entries = std::move(entries),
      .menuWidth = 280.0f * contentScale(),
      .maxVisible = 14,
      .anchor = PopupAnchorRect{
          .x = static_cast<std::int32_t>(
              std::round(std::clamp(static_cast<float>(requestX), 0.0f, static_cast<float>(parent->width)))
          ),
          .y = static_cast<std::int32_t>(
              std::round(std::clamp(static_cast<float>(requestY), 0.0f, static_cast<float>(parent->height)))
          ),
          .width = 1,
          .height = 1,
      },
      .parent = PopupSurfaceParent{
          .layerSurface = parent->layerSurface,
          .xdgSurface = parent->xdgSurface,
          .output = parent->output,
      },
  });
  if (!popup->isOpen()) {
    closeContextMenu();
  }
}

void WebPanel::closeContextMenu() {
  if (m_contextMenu != nullptr) {
    PanelManager::instance().clearActivePopup();
    m_contextMenu->close();
    m_contextMenu.reset();
  }
  if (!m_contextMenuRequest.has_value()) {
    return;
  }
  auto request = std::move(*m_contextMenuRequest);
  m_contextMenuRequest.reset();
  if (request.complete) {
    request.complete(std::nullopt);
  }
}

void WebPanel::showBrowserPopup(CefBrowserPopupRequest request) {
  if (request.session == nullptr || root() == nullptr) {
    return;
  }
  if (m_popupStack.size() >= kMaxPresentedPopupDepth) {
    request.session->close();
    return;
  }
  if (!m_popupStack.empty()) {
    auto& previous = m_popupStack.back();
    previous.surface->detach();
    previous.layer->setVisible(false);
  }

  auto layer = std::make_unique<Node>();
  auto* layerPtr = layer.get();
  layer->setClipChildren(true);
  layer->setZIndex(4 + static_cast<std::int32_t>(m_popupStack.size()));

  auto surface = std::make_unique<CefSurfaceNode>(*request.session);
  auto* surfacePtr = surface.get();
  surface->attach(
      [this]() {
        tracy_latency::redrawQueued();
        if (auto* host = surfaceHost(); host != nullptr) {
          host->requestUpdateOnly();
          host->requestRedraw();
        }
      },
      [this]() {
        if (auto* host = surfaceHost(); host != nullptr) {
          host->requestCallbackTick();
        }
      },
      [this]() {
        if (auto* host = surfaceHost(); host != nullptr) {
          host->inputDispatcher().refreshCursor();
        }
      }
  );
  layer->addChild(std::move(surface));

  Button* closePtr = nullptr;
  auto closeButton = ui::button({
      .out = &closePtr,
      .glyph = "close",
      .variant = ButtonVariant::Secondary,
      .onClick = [this]() { DeferredCall::callLater([this]() { closeBrowserPopup(true); }); },
  });
  closeButton->setZIndex(1);
  layer->addChild(std::move(closeButton));

  request.session->setPopupCreatedCallback(
      [this](CefBrowserPopupRequest nested) { showBrowserPopup(std::move(nested)); }
  );
  std::weak_ptr<CefBrowserSession> weakSession = request.session;
  request.session->setClosedCallback([this, weakSession]() {
    DeferredCall::callLater([this, weakSession]() {
      if (auto session = weakSession.lock()) {
        removeBrowserPopup(session.get(), false);
      }
    });
  });

  root()->addChild(std::move(layer));
  m_popupStack.push_back(PopupPresentation{
      .session = std::move(request.session),
      .layer = layerPtr,
      .surface = surfacePtr,
      .closeButton = closePtr,
      .preferredWidth = request.preferredWidth,
      .preferredHeight = request.preferredHeight,
  });
  if (m_surface != nullptr) {
    m_surface->inputArea()->setEnabled(false);
  }
  if (auto* host = surfaceHost(); host != nullptr) {
    host->requestLayout();
    host->requestUpdateOnly();
    host->requestRedraw();
    host->focusArea(surfacePtr->inputArea());
  }
}

void WebPanel::closeBrowserPopup(bool closeSession) {
  if (!m_popupStack.empty()) {
    removeBrowserPopup(m_popupStack.back().session.get(), closeSession);
  }
}

void WebPanel::removeBrowserPopup(CefBrowserSession* session, bool closeSession) {
  const auto found = std::find_if(
      m_popupStack.begin(), m_popupStack.end(),
      [session](const PopupPresentation& popup) { return popup.session.get() == session; }
  );
  if (found == m_popupStack.end()) {
    return;
  }
  const std::size_t target = static_cast<std::size_t>(std::distance(m_popupStack.begin(), found));
  while (m_popupStack.size() > target) {
    auto popup = std::move(m_popupStack.back());
    m_popupStack.pop_back();
    popup.session->setPopupCreatedCallback(nullptr);
    popup.session->setClosedCallback(nullptr);
    popup.surface->detach();
    if (root() != nullptr && popup.layer != nullptr) {
      (void)root()->removeChild(popup.layer);
    }
    if (closeSession || popup.session.get() != session) {
      popup.session->close();
    }
  }

  if (!m_popupStack.empty()) {
    auto& previous = m_popupStack.back();
    previous.layer->setVisible(true);
    previous.surface->attach(
        [this]() {
          tracy_latency::redrawQueued();
          if (auto* host = surfaceHost(); host != nullptr) {
            host->requestUpdateOnly();
            host->requestRedraw();
          }
        },
        [this]() {
          if (auto* host = surfaceHost(); host != nullptr) {
            host->requestCallbackTick();
          }
        },
        [this]() {
          if (auto* host = surfaceHost(); host != nullptr) {
            host->inputDispatcher().refreshCursor();
          }
        }
    );
    if (auto* host = surfaceHost(); host != nullptr) {
      host->focusArea(previous.surface->inputArea());
    }
  } else if (m_surface != nullptr) {
    m_surface->inputArea()->setEnabled(m_session->hasUsableFrame() && !m_permissionRequest.has_value());
    if (auto* host = surfaceHost(); host != nullptr) {
      host->focusArea(m_surface->inputArea());
    }
  }
  if (auto* host = surfaceHost(); host != nullptr) {
    host->requestLayout();
    host->requestUpdateOnly();
    host->requestRedraw();
  }
}
