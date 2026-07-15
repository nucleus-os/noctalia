#include "shell/apple_music/apple_music_panel.h"

#include "cef/cef_service.h"
#include "cef/cef_surface_node.h"
#include "core/tracy.h"
#include "core/tracy_latency.h"
#include "render/core/renderer.h"
#include "shell/panel/panel_manager.h"

#include <memory>

namespace {
  std::uint64_t g_cefFrameReadyRequests = 0;
}

AppleMusicPanel::AppleMusicPanel(CefService& service) : m_service(service) {}

void AppleMusicPanel::create() {
  auto surface = std::make_unique<CefSurfaceNode>(m_service);
  m_surface = surface.get();
  m_surface->attach(
      []() {
        NOCTALIA_TRACE_ZONE("Apple Music CEF frame-ready request");
        ++g_cefFrameReadyRequests;
        NOCTALIA_TRACE_PLOT(
            "Apple Music CEF frame-ready requests",
            static_cast<std::int64_t>(g_cefFrameReadyRequests)
        );
        tracy_latency::redrawQueued();
        // A CEF frame must run AppleMusicPanel::doUpdate() so the scene adopts
        // the new texture. requestFrameTick() only advances animations and can
        // leave the browser frozen until unrelated user input requests update.
        PanelManager::instance().requestUpdateOnly();
        PanelManager::instance().requestRedraw();
      },
      []() { PanelManager::instance().inputDispatcher().refreshCursor(); }
  );
  setRoot(std::move(surface));
}

void AppleMusicPanel::onOpen(std::string_view /*context*/) {
  if (!m_initialNavigationRequested) {
    m_service.navigate("https://music.apple.com/");
    m_initialNavigationRequested = true;
  }
  m_service.setDisplayAttached(true);
}

void AppleMusicPanel::onClose() {
  if (m_surface != nullptr) {
    m_surface->detach();
  }
}

void AppleMusicPanel::onFrameTick(float /*deltaMs*/) {
  m_service.onFrameOpportunity();
}

void AppleMusicPanel::onPresentation(const SurfacePresentationFeedback& feedback) {
  m_service.onPresentation(feedback);
}

InputArea* AppleMusicPanel::initialFocusArea() const {
  return m_surface != nullptr ? m_surface->inputArea() : nullptr;
}

void AppleMusicPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_surface == nullptr) {
    return;
  }
  m_surface->setPosition(0.0f, 0.0f);
  m_surface->setSize(width, height);
  m_surface->layout(renderer);
}

void AppleMusicPanel::doUpdate(Renderer& renderer) {
  NOCTALIA_TRACE_ZONE("Apple Music adopt CEF texture");
  if (m_surface != nullptr && m_surface->syncTexture(renderer.textureManager())) {
    m_surface->markPaintDirty();
  }
}
