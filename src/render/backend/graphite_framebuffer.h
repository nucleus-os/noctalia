#pragma once

#include "include/core/SkRefCnt.h"
#include "render/backend/graphite_texture_manager.h"
#include "render/backend/render_backend.h"
#include "render/core/texture_handle.h"

#include <cstdint>

class SkCanvas;
class SkSurface;

// Recorder-owned offscreen render target. Its stable TextureId is rebound to a
// fresh SkSurface snapshot whenever the surface generation changes, so callers
// can retain framebuffer handles without retaining stale image snapshots.
class GraphiteFramebuffer final : public RenderFramebuffer, private GraphiteTextureManagerObserver {
public:
  GraphiteFramebuffer(
      GraphiteTextureManager& textures, sk_sp<SkSurface> surface, std::uint32_t width, std::uint32_t height
  );
  ~GraphiteFramebuffer() override;

  GraphiteFramebuffer(const GraphiteFramebuffer&) = delete;
  GraphiteFramebuffer& operator=(const GraphiteFramebuffer&) = delete;

  [[nodiscard]] bool valid() const noexcept override;
  [[nodiscard]] TextureId colorTexture() const noexcept override;
  [[nodiscard]] std::uint32_t width() const noexcept override { return m_width; }
  [[nodiscard]] std::uint32_t height() const noexcept override { return m_height; }
  void abandon() noexcept override;

  [[nodiscard]] SkCanvas* canvas() const noexcept;
  [[nodiscard]] SkSurface* readbackSurfaceForTesting() const noexcept { return m_surface.get(); }
  [[nodiscard]] bool refreshSnapshot() const noexcept;

private:
  void onGraphiteTextureManagerInvalidated() noexcept override;

  GraphiteTextureManager* m_textures = nullptr;
  sk_sp<SkSurface> m_surface;
  mutable TextureHandle m_color;
  mutable std::uint32_t m_snapshotGeneration = 0;
  std::uint32_t m_width = 0;
  std::uint32_t m_height = 0;
};
