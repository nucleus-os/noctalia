#include "shell/apple_music/apple_music_panel.h"

#include "cef/cef_service.h"
#include "cef/cef_surface_node.h"
#include "render/core/renderer.h"
#include "shell/panel/panel_manager.h"

#include <memory>

AppleMusicPanel::AppleMusicPanel(CefService& service) : m_service(service) {}

void AppleMusicPanel::create() {
  auto surface = std::make_unique<CefSurfaceNode>(m_service);
  m_surface = surface.get();
  m_surface->attach([]() { PanelManager::instance().requestFrameTick(); });
  setRoot(std::move(surface));
}

void AppleMusicPanel::onOpen(std::string_view /*context*/) {
  m_service.navigate("https://music.apple.com/");
  m_service.setDisplayAttached(true);
}

void AppleMusicPanel::onClose() {
  if (m_surface != nullptr) {
    m_surface->detach();
  }
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
  if (m_surface != nullptr && m_surface->syncTexture(renderer.textureManager())) {
    m_surface->markPaintDirty();
  }
}
