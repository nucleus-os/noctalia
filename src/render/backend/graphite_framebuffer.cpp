#include "render/backend/graphite_framebuffer.h"

#include "include/core/SkImage.h"
#include "include/core/SkSurface.h"
#include "render/backend/graphite_texture_manager.h"

#include <utility>

GraphiteFramebuffer::GraphiteFramebuffer(
    GraphiteTextureManager& textures, sk_sp<SkSurface> surface, std::uint32_t width, std::uint32_t height
)
    : m_textures(&textures), m_surface(std::move(surface)), m_width(width), m_height(height) {
  textures.addObserver(*this);
  if (m_surface == nullptr || width == 0 || height == 0) {
    return;
  }
  sk_sp<SkImage> snapshot = m_surface->makeImageSnapshot();
  if (snapshot == nullptr) {
    return;
  }
  m_color = textures.adoptExternalImage(std::move(snapshot), static_cast<int>(width), static_cast<int>(height));
  if (m_color.valid()) {
    m_snapshotGeneration = m_surface->generationID();
  }
}

GraphiteFramebuffer::~GraphiteFramebuffer() {
  if (m_textures != nullptr) {
    m_textures->removeObserver(*this);
    m_textures->unload(m_color);
  }
  m_surface.reset();
}

bool GraphiteFramebuffer::valid() const noexcept {
  return m_surface != nullptr && m_textures != nullptr && m_color.valid() && m_textures->image(m_color.id) != nullptr;
}

void GraphiteFramebuffer::onGraphiteTextureManagerDestroying() noexcept {
  m_surface.reset();
  m_color = {};
  m_textures = nullptr;
  m_snapshotGeneration = 0;
}

void GraphiteFramebuffer::abandon() noexcept {
  if (m_textures != nullptr) {
    m_textures->removeObserver(*this);
  }
  m_surface.reset();
  m_color = {};
  m_textures = nullptr;
  m_snapshotGeneration = 0;
}

TextureId GraphiteFramebuffer::colorTexture() const noexcept { return refreshSnapshot() ? m_color.id : TextureId{}; }

SkCanvas* GraphiteFramebuffer::canvas() const noexcept {
  return m_surface != nullptr ? m_surface->getCanvas() : nullptr;
}

bool GraphiteFramebuffer::refreshSnapshot() const noexcept {
  if (!valid()) {
    return false;
  }
  const std::uint32_t generation = m_surface->generationID();
  if (generation == m_snapshotGeneration) {
    return true;
  }
  sk_sp<SkImage> snapshot = m_surface->makeImageSnapshot();
  if (snapshot == nullptr
      || !m_textures->rebindExternalImage(
          m_color, std::move(snapshot), static_cast<int>(m_width), static_cast<int>(m_height)
      )) {
    return false;
  }
  m_snapshotGeneration = generation;
  return true;
}
