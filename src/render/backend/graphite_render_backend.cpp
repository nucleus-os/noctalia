#include "render/backend/graphite_render_backend.h"

#include "core/log.h"
#include "core/tracy.h"
#include "core/tracy_latency.h"
#include "render/backend/graphite_surface_target.h"
#include "render/backend/graphite_texture_manager.h"
#include "render/core/render_styles.h"
#include "render/graphics_device.h"
#include "render/render_target.h"

#include "include/core/SkBlendMode.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkImage.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/effects/SkGradient.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

namespace {
  constexpr Logger kLog("graphite-render");

  class GraphiteRenderSurfaceTarget final : public RenderSurfaceTarget {
  public:
    GraphiteRenderSurfaceTarget(GraphicsDevice& graphics, wl_surface* surface) : m_target(graphics, surface) {}
    void resize(std::uint32_t width, std::uint32_t height) override { m_target.resize(width, height); }
    void setPresentationCallback(SurfacePresentationCallback callback) override {
      m_target.setPresentationCallback(std::move(callback));
    }
    void destroy() override { m_target.destroy(); }
    [[nodiscard]] bool isReady() const noexcept override { return m_target.ready(); }
    [[nodiscard]] GraphiteSurfaceTarget& target() noexcept { return m_target; }

  private:
    GraphiteSurfaceTarget m_target;
  };

  GraphiteRenderSurfaceTarget& surfaceTarget(RenderTarget& target) {
    auto* result = dynamic_cast<GraphiteRenderSurfaceTarget*>(target.surfaceTarget());
    if (result == nullptr || !result->isReady()) {
      throw std::runtime_error("Graphite backend received an incompatible or unready surface target");
    }
    return *result;
  }

  SkColor4f skColor(const Color& color) { return {color.r, color.g, color.b, color.a}; }

  SkMatrix skMatrix(const Mat3& value) {
    SkMatrix result;
    result.setAll(
        value.m[0], value.m[3], value.m[6], value.m[1], value.m[4], value.m[7], value.m[2], value.m[5], value.m[8]
    );
    return result;
  }

  SkRRect roundedRect(float width, float height, const Radii& radius) {
    const SkRect rect = SkRect::MakeWH(width, height);
    const SkVector radii[4] = {
        {radius.tl, radius.tl}, {radius.tr, radius.tr}, {radius.br, radius.br}, {radius.bl, radius.bl}};
    SkRRect result;
    result.setRectRadii(rect, radii);
    return result;
  }

  SkBlendMode blendMode(RenderBlendMode mode) {
    return mode == RenderBlendMode::Disabled ? SkBlendMode::kSrc : SkBlendMode::kSrcOver;
  }
}

GraphiteRenderBackend::GraphiteRenderBackend(GraphicsDevice& graphics) : m_graphics(graphics) {
  if (!graphics.valid()) {
    throw std::runtime_error("GraphiteRenderBackend requires an initialized GraphicsDevice");
  }
}

GraphiteRenderBackend::~GraphiteRenderBackend() { cleanup(); }

void GraphiteRenderBackend::initialize(GlSharedContext& /*shared*/) {}

void GraphiteRenderBackend::cleanup() {
  m_canvas = nullptr;
  m_target = nullptr;
  m_scissor.reset();
  m_frameSaved = false;
}

bool GraphiteRenderBackend::makeCurrent(RenderTarget& target) {
  return dynamic_cast<GraphiteRenderSurfaceTarget*>(target.surfaceTarget()) != nullptr;
}

bool GraphiteRenderBackend::makeCurrentNoSurface() { return m_graphics.valid(); }

bool GraphiteRenderBackend::beginFrame(RenderTarget& target) {
  NOCTALIA_TRACE_ZONE("Graphite backend begin frame");
  auto& surface = surfaceTarget(target).target();
  RenderFrameStatus status = RenderFrameStatus::Deferred;
  m_canvas = surface.beginFrame(status);
  if (m_canvas == nullptr && status == RenderFrameStatus::RecreateSwapchain) {
    surface.resize(target.bufferWidth(), target.bufferHeight());
    m_canvas = surface.beginFrame(status);
  }
  if (m_canvas == nullptr) {
    if (status == RenderFrameStatus::DeviceLost) {
      surface.abandonDevice();
      m_deviceLost = true;
      kLog.error("Graphite acquire reported VK_ERROR_DEVICE_LOST");
    }
    return false;
  }
  tracy_latency::graphiteFrameBegan();
  m_target = &target;
  m_scissor.reset();
  m_blendMode = RenderBlendMode::PremultipliedAlpha;
  m_externalSynchronizations.clear();
  m_canvas->save();
  m_frameSaved = true;
  const float scaleX = target.logicalWidth() > 0
      ? static_cast<float>(target.bufferWidth()) / static_cast<float>(target.logicalWidth())
      : 1.0f;
  const float scaleY = target.logicalHeight() > 0
      ? static_cast<float>(target.bufferHeight()) / static_cast<float>(target.logicalHeight())
      : 1.0f;
  m_canvas->scale(scaleX, scaleY);
  m_canvas->clear(SK_ColorTRANSPARENT);
  return true;
}

void GraphiteRenderBackend::endFrame(RenderTarget& target) {
  NOCTALIA_TRACE_ZONE("Graphite backend end frame");
  if (m_canvas == nullptr) {
    return;
  }
  if (m_frameSaved) {
    m_canvas->restore();
  }
  m_frameSaved = false;
  m_canvas = nullptr;
  m_target = nullptr;
  bool released = false;
  const RenderFrameStatus status = surfaceTarget(target).target().endFrame([this, &released] {
    releaseExternalTextures();
    released = true;
  });
  if (!released) {
    // If recording/submission failed, no sampling occurred, but an ownership
    // acquire may already have been queued and must still be balanced.
    releaseExternalTextures();
  }
  if (status == RenderFrameStatus::DeviceLost) {
    surfaceTarget(target).target().abandonDevice();
    m_deviceLost = true;
    kLog.error("Graphite presentation reported VK_ERROR_DEVICE_LOST");
  } else if (status == RenderFrameStatus::Failed || status == RenderFrameStatus::SurfaceLost) {
    kLog.warn("Graphite presentation failed with status {}", static_cast<int>(status));
  }
}

RenderGraphicsResetStatus GraphiteRenderBackend::graphicsResetStatus() {
  return m_deviceLost ? RenderGraphicsResetStatus::Unknown : RenderGraphicsResetStatus::NoError;
}

void GraphiteRenderBackend::invalidateGpuResources() { m_graphics.textureManager().invalidateAll(); }

std::unique_ptr<RenderSurfaceTarget> GraphiteRenderBackend::createSurfaceTarget(wl_surface* surface) {
  return std::make_unique<GraphiteRenderSurfaceTarget>(m_graphics, surface);
}

std::unique_ptr<RenderFramebuffer>
GraphiteRenderBackend::createFramebuffer(std::uint32_t /*width*/, std::uint32_t /*height*/) {
  unsupported("offscreen framebuffer");
}
void GraphiteRenderBackend::bindFramebuffer(const RenderFramebuffer& /*framebuffer*/) { unsupported("framebuffer bind"); }
void GraphiteRenderBackend::bindDefaultFramebuffer() {}
void GraphiteRenderBackend::setViewport(std::uint32_t /*width*/, std::uint32_t /*height*/) {}
void GraphiteRenderBackend::clear(Color color) {
  if (m_canvas != nullptr) {
    m_canvas->clear(skColor(color));
  }
}
void GraphiteRenderBackend::setBlendMode(RenderBlendMode mode) { m_blendMode = mode; }

int GraphiteRenderBackend::maxTextureSize() {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(m_graphics.physicalDevice(), &properties);
  return static_cast<int>(properties.limits.maxImageDimension2D);
}

void GraphiteRenderBackend::setScissor(RenderScissor scissor) { m_scissor = scissor; }
void GraphiteRenderBackend::disableScissor() { m_scissor.reset(); }

void GraphiteRenderBackend::beginDraw(const Mat3& transform) {
  if (m_canvas == nullptr) {
    throw std::runtime_error("Graphite draw issued outside a frame scope");
  }
  m_canvas->save();
  if (m_scissor.has_value() && m_target != nullptr) {
    const float scaleX = m_target->logicalWidth() > 0
        ? static_cast<float>(m_target->bufferWidth()) / static_cast<float>(m_target->logicalWidth())
        : 1.0f;
    const float scaleY = m_target->logicalHeight() > 0
        ? static_cast<float>(m_target->bufferHeight()) / static_cast<float>(m_target->logicalHeight())
        : 1.0f;
    const float left = static_cast<float>(m_scissor->x) / scaleX;
    const auto bufferHeight = static_cast<std::int32_t>(m_target->bufferHeight());
    const float top = static_cast<float>(bufferHeight - (m_scissor->y + m_scissor->height)) / scaleY;
    const float right = static_cast<float>(m_scissor->x + m_scissor->width) / scaleX;
    const float bottom = static_cast<float>(bufferHeight - m_scissor->y) / scaleY;
    m_canvas->clipRect(SkRect::MakeLTRB(left, top, right, bottom), SkClipOp::kIntersect, false);
  }
  m_canvas->concat(skMatrix(transform));
}

void GraphiteRenderBackend::endDraw() { m_canvas->restore(); }

void GraphiteRenderBackend::drawRect(
    float /*surfaceWidth*/, float /*surfaceHeight*/, float width, float height, const RoundedRectStyle& style,
    const Mat3& transform
) {
  if (width <= 0.0f || height <= 0.0f || style.fillMode == FillMode::None) {
    return;
  }
  if (style.invertFill || style.corners.tl == CornerShape::Concave || style.corners.tr == CornerShape::Concave
      || style.corners.br == CornerShape::Concave || style.corners.bl == CornerShape::Concave) {
    unsupported("concave/inverted rectangle SkRuntimeEffect");
  }
  beginDraw(transform);
  const SkRRect shape = roundedRect(width, height, style.radius);
  SkPaint paint;
  paint.setAntiAlias(!style.noAa);
  paint.setBlendMode(blendMode(m_blendMode));
  if (style.fillMode == FillMode::LinearGradient) {
    std::array<SkColor4f, 4> colors;
    std::array<float, 4> positions;
    for (std::size_t index = 0; index < colors.size(); ++index) {
      colors[index] = skColor(style.gradientStops[index].color);
      positions[index] = style.gradientStops[index].position;
    }
    const SkPoint points[2] = {
        {0.0f, 0.0f},
        {style.gradientDirection == GradientDirection::Horizontal ? width : 0.0f,
         style.gradientDirection == GradientDirection::Vertical ? height : 0.0f}};
    const SkGradient gradient(
        SkGradient::Colors(SkSpan(colors), SkSpan(positions), SkTileMode::kClamp),
        SkGradient::Interpolation{.fInPremul = SkGradient::Interpolation::InPremul::kYes}
    );
    paint.setShader(SkShaders::LinearGradient(points, gradient));
  } else {
    paint.setColor4f(skColor(style.fill));
  }
  if (style.outerShadow) {
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, std::max(0.5f, style.softness), true));
  }
  m_canvas->drawRRect(shape, paint);
  if (style.borderWidth > 0.0f && style.border.a > 0.0f) {
    SkPaint border;
    border.setAntiAlias(!style.noAa);
    border.setStyle(SkPaint::kStroke_Style);
    border.setStrokeWidth(style.borderWidth);
    border.setColor4f(skColor(style.border));
    border.setBlendMode(blendMode(m_blendMode));
    m_canvas->drawRRect(shape, border);
  }
  endDraw();
}

void GraphiteRenderBackend::drawImage(const RenderImageDraw& draw) {
  if (!prepareExternalTexture(draw.texture)) {
    return;
  }
  auto* image = m_graphics.textureManager().image(draw.texture);
  if (image == nullptr || draw.width <= 0.0f || draw.height <= 0.0f) {
    return;
  }
  const float sourceWidth = draw.textureWidth > 0.0f ? draw.textureWidth : static_cast<float>(image->width());
  const float sourceHeight = draw.textureHeight > 0.0f ? draw.textureHeight : static_cast<float>(image->height());
  SkRect source = SkRect::MakeWH(sourceWidth, sourceHeight);
  SkRect destination = SkRect::MakeWH(draw.width, draw.height);
  if (draw.fitMode != RenderImageFitMode::Stretch && sourceWidth > 0.0f && sourceHeight > 0.0f) {
    const float scale = draw.fitMode == RenderImageFitMode::Cover
        ? std::max(draw.width / sourceWidth, draw.height / sourceHeight)
        : std::min(draw.width / sourceWidth, draw.height / sourceHeight);
    if (draw.fitMode == RenderImageFitMode::Cover) {
      const float visibleWidth = draw.width / scale;
      const float visibleHeight = draw.height / scale;
      source = SkRect::MakeXYWH(
          (sourceWidth - visibleWidth) * 0.5f, (sourceHeight - visibleHeight) * 0.5f, visibleWidth, visibleHeight
      );
    } else {
      const float fittedWidth = sourceWidth * scale;
      const float fittedHeight = sourceHeight * scale;
      destination = SkRect::MakeXYWH(
          (draw.width - fittedWidth) * 0.5f, (draw.height - fittedHeight) * 0.5f, fittedWidth, fittedHeight
      );
    }
  }
  beginDraw(draw.transform);
  if (draw.radius > 0.0f) {
    m_canvas->clipRRect(
        SkRRect::MakeRectXY(SkRect::MakeWH(draw.width, draw.height), draw.radius, draw.radius),
        SkClipOp::kIntersect, true
    );
  }
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setAlphaf(std::clamp(draw.opacity * draw.tint.a, 0.0f, 1.0f));
  paint.setBlendMode(blendMode(m_blendMode));
  if (draw.monochromeTint || draw.alphaMaskTint) {
    paint.setColorFilter(SkColorFilters::Blend(skColor(draw.tint), nullptr, SkBlendMode::kSrcIn));
  }
  const SkSamplingOptions sampling = m_graphics.textureManager().filter(draw.texture) == TextureFilter::Nearest
      ? SkSamplingOptions(SkFilterMode::kNearest)
      : SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone);
  m_canvas->drawImageRect(image, source, destination, sampling, &paint, SkCanvas::kStrict_SrcRectConstraint);
  if (draw.borderWidth > 0.0f && draw.borderColor.a > 0.0f) {
    SkPaint border;
    border.setAntiAlias(true);
    border.setStyle(SkPaint::kStroke_Style);
    border.setStrokeWidth(draw.borderWidth);
    border.setColor4f(skColor(draw.borderColor));
    m_canvas->drawRRect(
        SkRRect::MakeRectXY(SkRect::MakeWH(draw.width, draw.height), draw.radius, draw.radius), border
    );
  }
  endDraw();
}

void GraphiteRenderBackend::drawGlyph(const RenderGlyphDraw& draw) {
  if (!prepareExternalTexture(draw.texture)) {
    return;
  }
  auto* image = m_graphics.textureManager().image(draw.texture);
  if (image == nullptr) {
    return;
  }
  const float imageWidth = static_cast<float>(image->width());
  const float imageHeight = static_cast<float>(image->height());
  const SkRect source = SkRect::MakeLTRB(
      draw.u0 * imageWidth, draw.v0 * imageHeight, draw.u1 * imageWidth, draw.v1 * imageHeight
  );
  beginDraw(draw.transform);
  SkPaint paint;
  paint.setAlphaf(draw.opacity);
  paint.setBlendMode(blendMode(m_blendMode));
  if (draw.tinted) {
    paint.setColorFilter(SkColorFilters::Blend(skColor(draw.tint), nullptr, SkBlendMode::kSrcIn));
  }
  m_canvas->drawImageRect(
      image, source, SkRect::MakeWH(draw.width, draw.height), SkSamplingOptions(SkFilterMode::kLinear), &paint,
      SkCanvas::kStrict_SrcRectConstraint
  );
  endDraw();
}

void GraphiteRenderBackend::drawSpinner(float, float, float, float, const SpinnerStyle&, const Mat3&) { unsupported("spinner SkRuntimeEffect"); }
void GraphiteRenderBackend::drawCountdownRing(float, float, float, float, const CountdownRingStyle&, const Mat3&) { unsupported("countdown-ring SkRuntimeEffect"); }
void GraphiteRenderBackend::drawScreenCorner(float, float, float, float, float, float, const ScreenCornerStyle&, const Mat3&) { unsupported("screen-corner SkRuntimeEffect"); }
void GraphiteRenderBackend::drawAudioSpectrum(float, float, float, float, float, float, const AudioSpectrumStyle&, std::span<const float>, const Mat3&) { unsupported("audio-spectrum SkRuntimeEffect"); }
void GraphiteRenderBackend::drawFancyAudioVisualizer(TextureId, int, float, float, float, float, const FancyAudioVisualizerStyle&, const Mat3&) { unsupported("fancy-visualizer SkRuntimeEffect"); }
void GraphiteRenderBackend::drawEffect(float, float, float, float, const EffectStyle&, const Mat3&) { unsupported("weather/effect SkRuntimeEffect"); }
void GraphiteRenderBackend::drawGraph(TextureId, int, float, float, float, float, const GraphStyle&, const Mat3&) { unsupported("graph SkRuntimeEffect"); }
void GraphiteRenderBackend::drawWallpaper(const WallpaperDrawParams&) { unsupported("wallpaper transition SkRuntimeEffect"); }

void GraphiteRenderBackend::drawFullscreenTexture(TextureId texture, bool flipY) {
  if (!prepareExternalTexture(texture)) {
    return;
  }
  auto* image = m_graphics.textureManager().image(texture);
  if (m_canvas == nullptr || image == nullptr || m_target == nullptr) {
    return;
  }
  m_canvas->save();
  if (flipY) {
    m_canvas->translate(0.0f, static_cast<float>(m_target->logicalHeight()));
    m_canvas->scale(1.0f, -1.0f);
  }
  m_canvas->drawImageRect(
      image, SkRect::MakeWH(static_cast<float>(m_target->logicalWidth()), static_cast<float>(m_target->logicalHeight())),
      SkSamplingOptions(SkFilterMode::kLinear), nullptr
  );
  m_canvas->restore();
}

void GraphiteRenderBackend::drawFullscreenTint(Color color) { clear(color); }
void GraphiteRenderBackend::drawFramebufferBlur(TextureId, std::uint32_t, std::uint32_t, float, float, float) { unsupported("offscreen blur"); }

TextureManager& GraphiteRenderBackend::textureManager() { return m_graphics.textureManager(); }

bool GraphiteRenderBackend::prepareExternalTexture(TextureId texture) {
  auto* synchronization = m_graphics.textureManager().externalSynchronization(texture);
  if (synchronization == nullptr
      || std::ranges::find(m_externalSynchronizations, synchronization)
          != m_externalSynchronizations.end()) {
    return true;
  }
  if (!synchronization->prepareForGraphiteSampling()) {
    return false;
  }
  m_externalSynchronizations.push_back(synchronization);
  return true;
}

void GraphiteRenderBackend::releaseExternalTextures() {
  for (auto* synchronization : m_externalSynchronizations) {
    synchronization->releaseAfterGraphiteSampling();
  }
  m_externalSynchronizations.clear();
}

void GraphiteRenderBackend::unsupported(const char* operation) {
  throw std::runtime_error(std::string("Graphite production backend has not ported ") + operation);
}

std::unique_ptr<RenderBackend> createGraphiteRenderBackend(GraphicsDevice& graphics) {
  return std::make_unique<GraphiteRenderBackend>(graphics);
}
