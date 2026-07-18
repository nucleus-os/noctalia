#pragma once

#include "render/backend/graphite_runtime_effect.h"
#include "render/backend/graphite_texture_manager.h"
#include "render/backend/render_backend.h"

#include <optional>
#include <string>
#include <vector>

class GraphicsDevice;
class GraphiteFramebuffer;
class SkCanvas;

// RenderBackend adapter for the process-wide Vulkan/Graphite device.
class GraphiteRenderBackend final : public RenderBackend {
public:
  explicit GraphiteRenderBackend(GraphicsDevice& graphics);
  ~GraphiteRenderBackend() override;

  void cleanup() override;
  bool selectTarget(RenderTarget& target) override;
  bool beginFrame(RenderTarget& target) override;
  void endFrame(RenderTarget& target) override;
  [[nodiscard]] RenderDeviceStatus deviceStatus() const noexcept override;
  void invalidateGpuResources() override;
  void abandonAfterGraphicsReset() noexcept override;

  [[nodiscard]] std::unique_ptr<RenderSurfaceTarget> createSurfaceTarget(wl_surface* surface) override;
  [[nodiscard]] std::unique_ptr<RenderFramebuffer>
  createFramebuffer(std::uint32_t width, std::uint32_t height) override;
  void beginOffscreenFrame(const RenderFramebuffer& framebuffer) override;
  void endOffscreenFrame() override;
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
  void drawParagraph(std::uint64_t handle, float x, float y, const Mat3& transform) override;
  void drawTintedParagraph(std::uint64_t handle, float x, float y, const Color& color, const Mat3& transform) override;
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
  void drawFullscreenTexture(TextureId texture) override;
  void drawFullscreenTint(Color color) override;
  void drawFramebufferBlur(
      TextureId sourceTexture, std::uint32_t width, std::uint32_t height, float directionX, float directionY,
      float radius
  ) override;
  [[nodiscard]] TextureManager& textureManager() override;

private:
  struct ExternalSubmission {
    GraphiteExternalImageSynchronization* synchronization = nullptr;
    GraphiteSubmissionDependency dependency;
  };

  void beginDraw(const Mat3& transform);
  void endDraw();
  [[nodiscard]] bool prepareExternalTexture(TextureId texture);
  void finishExternalTextures(bool submitted);

  GraphicsDevice& m_graphics;
  SkCanvas* m_canvas = nullptr;
  SkCanvas* m_surfaceCanvas = nullptr;
  const GraphiteFramebuffer* m_boundFramebuffer = nullptr;
  RenderTarget* m_target = nullptr;
  RenderBlendMode m_blendMode = RenderBlendMode::PremultipliedAlpha;
  std::optional<RenderScissor> m_scissor;
  bool m_frameSaved = false;
  bool m_deviceLost = false;
  GraphiteRuntimeEffects m_runtimeEffects;
  std::vector<ExternalSubmission> m_externalSubmissions;
};
