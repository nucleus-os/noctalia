#pragma once

#include "render/backend/render_backend.h"

#include <optional>
#include <vector>

class GraphicsDevice;
class GraphiteExternalImageSynchronization;
class SkCanvas;

// Production RenderBackend adapter for the process-wide Vulkan/Graphite
// device. The first cut-over deliberately covers the ordinary scene
// primitives used by the CEF panel; specialized effects remain on GLES until
// their cached SkRuntimeEffect ports land.
class GraphiteRenderBackend final : public RenderBackend {
public:
  explicit GraphiteRenderBackend(GraphicsDevice& graphics);
  ~GraphiteRenderBackend() override;

  void initialize(GlSharedContext& shared) override;
  void cleanup() override;
  bool makeCurrent(RenderTarget& target) override;
  bool makeCurrentNoSurface() override;
  bool beginFrame(RenderTarget& target) override;
  void endFrame(RenderTarget& target) override;
  [[nodiscard]] RenderGraphicsResetStatus graphicsResetStatus() override;
  void invalidateGpuResources() override;

  [[nodiscard]] std::unique_ptr<RenderSurfaceTarget> createSurfaceTarget(wl_surface* surface) override;
  [[nodiscard]] std::unique_ptr<RenderFramebuffer>
  createFramebuffer(std::uint32_t width, std::uint32_t height) override;
  void bindFramebuffer(const RenderFramebuffer& framebuffer) override;
  void bindDefaultFramebuffer() override;
  void setViewport(std::uint32_t width, std::uint32_t height) override;
  void clear(Color color) override;
  void setBlendMode(RenderBlendMode mode) override;
  [[nodiscard]] int maxTextureSize() override;
  void setScissor(RenderScissor scissor) override;
  void disableScissor() override;
  void drawRect(
      float surfaceWidth, float surfaceHeight, float width, float height, const RoundedRectStyle& style,
      const Mat3& transform
  ) override;
  void drawImage(const RenderImageDraw& draw) override;
  void drawGlyph(const RenderGlyphDraw& draw) override;
  void drawSpinner(
      float surfaceWidth, float surfaceHeight, float width, float height, const SpinnerStyle& style,
      const Mat3& transform
  ) override;
  void drawCountdownRing(
      float surfaceWidth, float surfaceHeight, float width, float height, const CountdownRingStyle& style,
      const Mat3& transform
  ) override;
  void drawScreenCorner(
      float surfaceWidth, float surfaceHeight, float pixelScaleX, float pixelScaleY, float width, float height,
      const ScreenCornerStyle& style, const Mat3& transform
  ) override;
  void drawAudioSpectrum(
      float surfaceWidth, float surfaceHeight, float pixelScaleX, float pixelScaleY, float width, float height,
      const AudioSpectrumStyle& style, std::span<const float> values, const Mat3& transform
  ) override;
  void drawFancyAudioVisualizer(
      TextureId audioTexture, int textureWidth, float surfaceWidth, float surfaceHeight, float width, float height,
      const FancyAudioVisualizerStyle& style, const Mat3& transform
  ) override;
  void drawEffect(
      float surfaceWidth, float surfaceHeight, float width, float height, const EffectStyle& style,
      const Mat3& transform
  ) override;
  void drawGraph(
      TextureId dataTexture, int textureWidth, float surfaceWidth, float surfaceHeight, float width, float height,
      const GraphStyle& style, const Mat3& transform
  ) override;
  void drawWallpaper(const WallpaperDrawParams& params) override;
  void drawFullscreenTexture(TextureId texture, bool flipY) override;
  void drawFullscreenTint(Color color) override;
  void drawFramebufferBlur(
      TextureId sourceTexture, std::uint32_t width, std::uint32_t height, float directionX, float directionY,
      float radius
  ) override;
  [[nodiscard]] TextureManager& textureManager() override;

private:
  void beginDraw(const Mat3& transform);
  void endDraw();
  [[nodiscard]] bool prepareExternalTexture(TextureId texture);
  void releaseExternalTextures();
  [[noreturn]] static void unsupported(const char* operation);

  GraphicsDevice& m_graphics;
  SkCanvas* m_canvas = nullptr;
  RenderTarget* m_target = nullptr;
  RenderBlendMode m_blendMode = RenderBlendMode::PremultipliedAlpha;
  std::optional<RenderScissor> m_scissor;
  bool m_frameSaved = false;
  bool m_deviceLost = false;
  std::vector<GraphiteExternalImageSynchronization*> m_externalSynchronizations;
};
