#include "render/backend/graphite_render_backend.h"

#include <nucleus/text/TextLayoutBuilder.hpp>

#include "core/log.h"
#include "core/tracy.h"
#include "core/tracy_latency.h"
#include "include/core/SkBlendMode.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkM44.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkGradient.h"
#include "include/effects/SkImageFilters.h"
#include "include/gpu/graphite/Surface.h"
#include "render/backend/graphite_framebuffer.h"
#include "render/backend/graphite_runtime_effect.h"
#include "render/backend/graphite_surface_target.h"
#include "render/backend/graphite_texture_manager.h"
#include "render/core/render_styles.h"
#include "render/core/wallpaper_types.h"
#include "render/graphics_device.h"
#include "render/render_target.h"

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
    void abandonAfterDeviceLoss() noexcept override { m_target.abandonDevice(); }
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

  Color interpolateColor(const Color& low, const Color& high, float amount) {
    const float t = std::clamp(amount, 0.0f, 1.0f);
    return {
        .r = low.r + (high.r - low.r) * t,
        .g = low.g + (high.g - low.g) * t,
        .b = low.b + (high.b - low.b) * t,
        .a = low.a + (high.a - low.a) * t,
    };
  }

  float snapToPixel(float value, float pixelScale) { return std::floor(value * pixelScale + 0.5f) / pixelScale; }

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
        {radius.tl, radius.tl}, {radius.tr, radius.tr}, {radius.br, radius.br}, {radius.bl, radius.bl}
    };
    SkRRect result;
    result.setRectRadii(rect, radii);
    return result;
  }

  SkBlendMode blendMode(RenderBlendMode mode) {
    return mode == RenderBlendMode::Disabled ? SkBlendMode::kSrc : SkBlendMode::kSrcOver;
  }

  SkSamplingOptions imageSampling(const SkImage& image, TextureFilter filter = TextureFilter::Linear) {
    if (filter == TextureFilter::Nearest) {
      return SkSamplingOptions(SkFilterMode::kNearest);
    }
    return SkSamplingOptions(
        SkFilterMode::kLinear, image.hasMipmaps() ? SkMipmapMode::kLinear : SkMipmapMode::kNone
    );
  }
} // namespace

GraphiteRenderBackend::GraphiteRenderBackend(GraphicsDevice& graphics) : m_graphics(graphics) {
  if (!graphics.valid()) {
    throw std::runtime_error("GraphiteRenderBackend requires an initialized GraphicsDevice");
  }
}

GraphiteRenderBackend::~GraphiteRenderBackend() { cleanup(); }

void GraphiteRenderBackend::cleanup() {
  m_canvas = nullptr;
  m_surfaceCanvas = nullptr;
  m_boundFramebuffer = nullptr;
  m_target = nullptr;
  m_scissor.reset();
  m_frameSaved = false;
}

bool GraphiteRenderBackend::selectTarget(RenderTarget& target) {
  return dynamic_cast<GraphiteRenderSurfaceTarget*>(target.surfaceTarget()) != nullptr;
}

bool GraphiteRenderBackend::beginFrame(RenderTarget& target) {
  NOCTALIA_TRACE_ZONE("Graphite backend begin frame");
  if (m_deviceLost) {
    return false;
  }
  auto& surface = surfaceTarget(target).target();
  RenderFrameStatus status = RenderFrameStatus::Deferred;
  m_surfaceCanvas = surface.beginFrame(status);
  if (m_surfaceCanvas == nullptr && status == RenderFrameStatus::RecreateSwapchain) {
    surface.resize(target.bufferWidth(), target.bufferHeight());
    m_surfaceCanvas = surface.beginFrame(status);
  }
  if (m_surfaceCanvas == nullptr) {
    if (status == RenderFrameStatus::DeviceLost) {
      surface.abandonDevice();
      m_deviceLost = true;
      kLog.error("Graphite acquire reported VK_ERROR_DEVICE_LOST");
    }
    return false;
  }
  m_canvas = m_surfaceCanvas;
  m_boundFramebuffer = nullptr;
  tracy_latency::graphiteFrameBegan();
  m_target = &target;
  m_scissor.reset();
  m_blendMode = RenderBlendMode::PremultipliedAlpha;
  m_externalSubmissions.clear();
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
  endOffscreenFrame();
  if (m_frameSaved) {
    m_canvas->restore();
  }
  m_frameSaved = false;
  m_canvas = nullptr;
  m_surfaceCanvas = nullptr;
  m_boundFramebuffer = nullptr;
  m_target = nullptr;
  std::vector<VkSemaphore> externalWaits;
  std::vector<VkSemaphore> externalSignals;
  externalWaits.reserve(m_externalSubmissions.size());
  externalSignals.reserve(m_externalSubmissions.size());
  for (const auto& submission : m_externalSubmissions) {
    if (submission.dependency.waitSemaphore != VK_NULL_HANDLE) {
      externalWaits.push_back(submission.dependency.waitSemaphore);
    }
    if (submission.dependency.signalSemaphore != VK_NULL_HANDLE) {
      externalSignals.push_back(submission.dependency.signalSemaphore);
    }
  }
  bool submitted = false;
  const RenderFrameStatus status = surfaceTarget(target).target().endFrame(
      externalWaits, externalSignals,
      [this, &submitted] {
        finishExternalTextures(true);
        submitted = true;
      }
  );
  if (!submitted) {
    // If recording/submission failed, no sampling occurred, but an ownership
    // acquire may already have been queued and must still be balanced.
    finishExternalTextures(false);
  }
  if (status == RenderFrameStatus::DeviceLost) {
    surfaceTarget(target).target().abandonDevice();
    m_deviceLost = true;
    kLog.error("Graphite presentation reported VK_ERROR_DEVICE_LOST");
  } else if (status == RenderFrameStatus::Failed || status == RenderFrameStatus::SurfaceLost) {
    kLog.warn("Graphite presentation failed with status {}", static_cast<int>(status));
  }
}

RenderDeviceStatus GraphiteRenderBackend::deviceStatus() const noexcept {
  return m_deviceLost ? RenderDeviceStatus::Lost : RenderDeviceStatus::Ready;
}

void GraphiteRenderBackend::abandonGpuResourcesAfterDeviceLoss() noexcept {
  m_graphics.abandonAfterDeviceLoss();
  cleanup();
  m_deviceLost = true;
}

std::unique_ptr<RenderSurfaceTarget> GraphiteRenderBackend::createSurfaceTarget(wl_surface* surface) {
  return std::make_unique<GraphiteRenderSurfaceTarget>(m_graphics, surface);
}

std::unique_ptr<RenderFramebuffer> GraphiteRenderBackend::createFramebuffer(std::uint32_t width, std::uint32_t height) {
  if (width == 0
      || height == 0
      || m_graphics.recorder() == nullptr
      || width > static_cast<std::uint32_t>(maxTextureSize())
      || height > static_cast<std::uint32_t>(maxTextureSize())) {
    return nullptr;
  }
  const SkImageInfo info = SkImageInfo::Make(
      static_cast<int>(width), static_cast<int>(height), kRGBA_8888_SkColorType, kPremul_SkAlphaType,
      SkColorSpace::MakeSRGB()
  );
  sk_sp<SkSurface> surface = SkSurfaces::RenderTarget(m_graphics.recorder(), info);
  if (surface == nullptr) {
    return nullptr;
  }
  auto framebuffer =
      std::make_unique<GraphiteFramebuffer>(m_graphics.textureManager(), std::move(surface), width, height);
  return framebuffer->valid() ? std::move(framebuffer) : nullptr;
}
void GraphiteRenderBackend::beginOffscreenFrame(const RenderFramebuffer& framebuffer) {
  const auto* graphiteFramebuffer = dynamic_cast<const GraphiteFramebuffer*>(&framebuffer);
  if (graphiteFramebuffer == nullptr || !graphiteFramebuffer->valid() || graphiteFramebuffer->canvas() == nullptr) {
    throw std::runtime_error("Graphite backend received an incompatible or invalid framebuffer");
  }
  if (m_boundFramebuffer != nullptr) {
    throw std::runtime_error("Graphite backend received a nested offscreen frame scope");
  }
  m_boundFramebuffer = graphiteFramebuffer;
  m_canvas = graphiteFramebuffer->canvas();
}
void GraphiteRenderBackend::endOffscreenFrame() {
  if (m_boundFramebuffer != nullptr) {
    (void)m_boundFramebuffer->refreshSnapshot();
  }
  m_boundFramebuffer = nullptr;
  m_canvas = m_surfaceCanvas;
}
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
  if (m_scissor.has_value() && (m_target != nullptr || m_boundFramebuffer != nullptr)) {
    const float scaleX = m_target != nullptr && m_target->logicalWidth() > 0
        ? static_cast<float>(m_target->bufferWidth()) / static_cast<float>(m_target->logicalWidth())
        : 1.0f;
    const float scaleY = m_target != nullptr && m_target->logicalHeight() > 0
        ? static_cast<float>(m_target->bufferHeight()) / static_cast<float>(m_target->logicalHeight())
        : 1.0f;
    const float left = static_cast<float>(m_scissor->x) / scaleX;
    const auto bufferHeight = static_cast<std::int32_t>(
        m_target != nullptr ? m_target->bufferHeight() : m_boundFramebuffer->height()
    );
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
  if (width <= 0.0f || height <= 0.0f) {
    return;
  }
  const bool hasConcaveCorner = style.corners.tl == CornerShape::Concave
      || style.corners.tr == CornerShape::Concave
      || style.corners.br == CornerShape::Concave
      || style.corners.bl == CornerShape::Concave;
  const bool hasLogicalInset = style.logicalInset.left != 0.0f
      || style.logicalInset.top != 0.0f
      || style.logicalInset.right != 0.0f
      || style.logicalInset.bottom != 0.0f;
  const bool advanced =
      style.invertFill || hasConcaveCorner || hasLogicalInset || style.outerShadow || style.shadowExclusion;
  if (advanced) {
    const float padding = std::max(style.borderWidth + style.softness + 2.0f, 2.0f);
    const float quadWidth = width + padding * 2.0f;
    const float quadHeight = height + padding * 2.0f;
    const auto cornerShape = [](CornerShape shape) { return shape == CornerShape::Concave ? 1.0f : 0.0f; };

    SkRuntimeShaderBuilder builder(m_runtimeEffects.get(GraphiteRuntimeEffectId::AdvancedRect));
    builder.uniform("u_rect_size") = SkV2{width, height};
    builder.uniform("u_rect_origin") = SkV2{padding, padding};
    builder.uniform("u_color") = skColor(style.fill);
    builder.uniform("u_border_color") = skColor(style.border);
    builder.uniform("u_fill_mode") = static_cast<float>(style.fillMode);
    builder.uniform("u_gradient_direction") = SkV2{
        style.gradientDirection == GradientDirection::Horizontal ? 1.0f : 0.0f,
        style.gradientDirection == GradientDirection::Vertical ? 1.0f : 0.0f
    };
    builder.uniform("u_gradient_stops") = SkV4{
        style.gradientStops[0].position, style.gradientStops[1].position, style.gradientStops[2].position,
        style.gradientStops[3].position
    };
    builder.uniform("u_gradient_color0") = skColor(style.gradientStops[0].color);
    builder.uniform("u_gradient_color1") = skColor(style.gradientStops[1].color);
    builder.uniform("u_gradient_color2") = skColor(style.gradientStops[2].color);
    builder.uniform("u_gradient_color3") = skColor(style.gradientStops[3].color);
    builder.uniform("u_corner_shapes") = SkV4{
        cornerShape(style.corners.tl), cornerShape(style.corners.tr), cornerShape(style.corners.br),
        cornerShape(style.corners.bl)
    };
    builder.uniform("u_logical_inset") =
        SkV4{style.logicalInset.left, style.logicalInset.top, style.logicalInset.right, style.logicalInset.bottom};
    builder.uniform("u_radii") = SkV4{style.radius.tl, style.radius.tr, style.radius.br, style.radius.bl};
    builder.uniform("u_softness") = style.softness;
    builder.uniform("u_no_aa") = style.noAa ? 1.0f : 0.0f;
    builder.uniform("u_invert_fill") = style.invertFill ? 1.0f : 0.0f;
    builder.uniform("u_border_width") = style.borderWidth;
    builder.uniform("u_outer_shadow") = style.outerShadow ? 1.0f : 0.0f;
    builder.uniform("u_shadow_cutout_offset") = SkV2{style.shadowCutoutOffsetX, style.shadowCutoutOffsetY};
    builder.uniform("u_shadow_exclusion") = style.shadowExclusion ? 1.0f : 0.0f;
    builder.uniform("u_shadow_exclusion_offset") = SkV2{style.shadowExclusionOffsetX, style.shadowExclusionOffsetY};
    builder.uniform("u_shadow_exclusion_size") = SkV2{style.shadowExclusionWidth, style.shadowExclusionHeight};
    builder.uniform("u_shadow_exclusion_corner_shapes") = SkV4{
        cornerShape(style.shadowExclusionCorners.tl), cornerShape(style.shadowExclusionCorners.tr),
        cornerShape(style.shadowExclusionCorners.br), cornerShape(style.shadowExclusionCorners.bl)
    };
    builder.uniform("u_shadow_exclusion_logical_inset") = SkV4{
        style.shadowExclusionLogicalInset.left, style.shadowExclusionLogicalInset.top,
        style.shadowExclusionLogicalInset.right, style.shadowExclusionLogicalInset.bottom
    };
    builder.uniform("u_shadow_exclusion_radii") = SkV4{
        style.shadowExclusionRadius.tl, style.shadowExclusionRadius.tr, style.shadowExclusionRadius.br,
        style.shadowExclusionRadius.bl
    };
    sk_sp<SkShader> shader = builder.makeShader();
    if (shader == nullptr) {
      throw std::runtime_error("failed to instantiate Graphite advanced rectangle shader");
    }

    beginDraw(transform * Mat3::translation(-padding, -padding));
    SkPaint paint;
    paint.setAntiAlias(false);
    paint.setBlendMode(blendMode(m_blendMode));
    paint.setShader(std::move(shader));
    m_canvas->drawRect(SkRect::MakeWH(quadWidth, quadHeight), paint);
    endDraw();
    return;
  }
  if (style.fillMode == FillMode::None && (style.borderWidth <= 0.0f || style.border.a <= 0.0f)) {
    return;
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
         style.gradientDirection == GradientDirection::Vertical ? height : 0.0f}
    };
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
        SkRRect::MakeRectXY(SkRect::MakeWH(draw.width, draw.height), draw.radius, draw.radius), SkClipOp::kIntersect,
        true
    );
  }
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setAlphaf(std::clamp(draw.opacity * draw.tint.a, 0.0f, 1.0f));
  paint.setBlendMode(blendMode(m_blendMode));
  if (draw.monochromeTint || draw.alphaMaskTint) {
    paint.setColorFilter(SkColorFilters::Blend(skColor(draw.tint), nullptr, SkBlendMode::kSrcIn));
  }
  const SkSamplingOptions sampling = imageSampling(*image, m_graphics.textureManager().filter(draw.texture));
  m_canvas->drawImageRect(image, source, destination, sampling, &paint, SkCanvas::kStrict_SrcRectConstraint);
  if (draw.borderWidth > 0.0f && draw.borderColor.a > 0.0f) {
    SkPaint border;
    border.setAntiAlias(true);
    border.setStyle(SkPaint::kStroke_Style);
    border.setStrokeWidth(draw.borderWidth);
    border.setColor4f(skColor(draw.borderColor));
    m_canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeWH(draw.width, draw.height), draw.radius, draw.radius), border);
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
  const SkRect source =
      SkRect::MakeLTRB(draw.u0 * imageWidth, draw.v0 * imageHeight, draw.u1 * imageWidth, draw.v1 * imageHeight);
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

void GraphiteRenderBackend::drawParagraph(std::uint64_t handle, float x, float y, const Mat3& transform) {
  if (handle == 0) {
    return;
  }
  beginDraw(transform);
  const nucleus::text::TextLayoutService service;
  if (!service.paint(handle, m_canvas, x, y)) {
    endDraw();
    throw std::runtime_error("Graphite backend received an invalid paragraph handle");
  }
  endDraw();
}

void GraphiteRenderBackend::drawTintedParagraph(
    std::uint64_t handle, float x, float y, const Color& color, const Mat3& transform) {
  if (handle == 0) return;
  beginDraw(transform);
  SkPaint layer;
  layer.setColorFilter(SkColorFilters::Blend(skColor(color), nullptr, SkBlendMode::kSrcIn));
  m_canvas->saveLayer(nullptr, &layer);
  const nucleus::text::TextLayoutService service;
  const bool painted = service.paint(handle, m_canvas, x, y);
  m_canvas->restore();
  endDraw();
  if (!painted) throw std::runtime_error("Graphite backend received an invalid paragraph handle");
}


void GraphiteRenderBackend::drawSpinner(
    float /*surfaceWidth*/, float /*surfaceHeight*/, float width, float height, const SpinnerStyle& style,
    const Mat3& transform
) {
  const float thickness = std::clamp(style.thickness, 0.0f, std::min(width, height));
  if (width <= 0.0f || height <= 0.0f || thickness <= 0.0f || style.color.a <= 0.0f) {
    return;
  }

  beginDraw(transform);
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(thickness);
  paint.setStrokeCap(SkPaint::kButt_Cap);
  paint.setColor4f(skColor(style.color));
  paint.setBlendMode(blendMode(m_blendMode));
  const float inset = thickness * 0.5f;
  const SkRect oval = SkRect::MakeLTRB(inset, inset, width - inset, height - inset);
  // Preserve the established visual: a 90-degree notch centered on the
  // positive X axis.
  m_canvas->drawArc(oval, 45.0f, 270.0f, false, paint);
  endDraw();
}

void GraphiteRenderBackend::drawCountdownRing(
    float /*surfaceWidth*/, float /*surfaceHeight*/, float width, float height, const CountdownRingStyle& style,
    const Mat3& transform
) {
  const float thickness = std::clamp(style.thickness, 0.0f, std::min(width, height));
  const float progress = std::clamp(style.progress, 0.0f, 1.0f);
  if (width <= 0.0f || height <= 0.0f || thickness <= 0.0f || progress <= 0.0f || style.color.a <= 0.0f) {
    return;
  }

  beginDraw(transform);
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(thickness);
  paint.setStrokeCap(SkPaint::kButt_Cap);
  paint.setColor4f(skColor(style.color));
  paint.setBlendMode(blendMode(m_blendMode));
  const float inset = thickness * 0.5f;
  const SkRect oval = SkRect::MakeLTRB(inset, inset, width - inset, height - inset);
  m_canvas->drawArc(oval, -90.0f, progress * 360.0f, false, paint);
  endDraw();
}
void GraphiteRenderBackend::drawScreenCorner(
    float /*surfaceWidth*/, float /*surfaceHeight*/, float pixelScaleX, float pixelScaleY, float width, float height,
    const ScreenCornerStyle& style, const Mat3& transform
) {
  if (width <= 0.0f || height <= 0.0f || style.color.a <= 0.0f) {
    return;
  }

  SkRuntimeShaderBuilder builder(m_runtimeEffects.get(GraphiteRuntimeEffectId::ScreenCorner));
  builder.uniform("size") = SkPoint::Make(width, height);
  builder.uniform("pixelScale") = SkPoint::Make(std::max(1.0f, pixelScaleX), std::max(1.0f, pixelScaleY));
  builder.uniform("color") = skColor(style.color);
  builder.uniform("corner") = static_cast<std::int32_t>(style.position);
  builder.uniform("exponent") = std::max(1.0f, style.exponent);
  builder.uniform("softness") = std::max(0.0f, style.softness);
  sk_sp<SkShader> shader = builder.makeShader();
  if (shader == nullptr) {
    throw std::runtime_error("failed to instantiate Graphite screen-corner shader");
  }

  beginDraw(transform);
  SkPaint paint;
  paint.setAntiAlias(false);
  paint.setBlendMode(blendMode(m_blendMode));
  paint.setShader(std::move(shader));
  m_canvas->drawRect(SkRect::MakeWH(width, height), paint);
  endDraw();
}
void GraphiteRenderBackend::drawAudioSpectrum(
    float /*surfaceWidth*/, float /*surfaceHeight*/, float pixelScaleX, float pixelScaleY, float width, float height,
    const AudioSpectrumStyle& style, std::span<const float> values, const Mat3& transform
) {
  constexpr float kGapToBarRatio = 0.5f;
  if (width <= 0.0f || height <= 0.0f || values.empty()) {
    return;
  }
  const int valueCount = static_cast<int>(values.size());
  const int barCount = style.mirrored ? valueCount * 2 : valueCount;
  if (barCount <= 0) {
    return;
  }

  const bool horizontal = style.orientation == AudioSpectrumOrientation::Horizontal;
  const float safeScaleX = std::max(0.001f, pixelScaleX);
  const float safeScaleY = std::max(0.001f, pixelScaleY);
  const float mainScale = horizontal ? safeScaleX : safeScaleY;
  const float crossScale = horizontal ? safeScaleY : safeScaleX;
  const float mainLength = horizontal ? width : height;
  const float crossLength = horizontal ? height : width;
  const int gapCount = std::max(0, barCount - 1);
  const float weightedSlots = static_cast<float>(barCount) + static_cast<float>(gapCount) * kGapToBarRatio;
  const float devicePixel = 1.0f / mainScale;
  const bool compact = mainLength * mainScale < static_cast<float>(barCount + gapCount);
  const float barThickness = compact
      ? mainLength / static_cast<float>(barCount)
      : std::max(devicePixel, std::floor(mainLength / std::max(1.0f, weightedSlots) * mainScale) / mainScale);
  const float gapThickness = compact || gapCount == 0
      ? 0.0f
      : std::max(devicePixel, std::floor(barThickness * kGapToBarRatio * mainScale) / mainScale);
  const float stride = barThickness + gapThickness;
  const float used = barThickness * static_cast<float>(barCount) + gapThickness * static_cast<float>(gapCount);
  const float startOffset = std::floor(std::max(0.0f, (mainLength - used) * 0.5f) * mainScale) / mainScale;

  beginDraw(transform);
  SkPaint paint;
  paint.setAntiAlias(false);
  paint.setBlendMode(blendMode(m_blendMode));
  for (int index = 0; index < barCount; ++index) {
    const int valueIndex = style.mirrored ? (index < valueCount ? valueCount - 1 - index : index - valueCount) : index;
    const float value = std::clamp(values[static_cast<std::size_t>(valueIndex)], 0.0f, 1.0f);
    float crossPixels = std::max(1.0f, std::floor(value * crossLength * crossScale + 0.5f));
    if (style.centered && crossPixels > 1.0f) {
      crossPixels = std::max(2.0f, std::round(crossPixels * 0.5f) * 2.0f);
    }
    const float crossSize = crossPixels / crossScale;
    float mainStart = compact ? startOffset + static_cast<float>(index) * stride
                              : snapToPixel(startOffset + static_cast<float>(index) * stride, mainScale);
    float mainEnd = mainStart + barThickness;
    if (mainStart < 0.0f) {
      mainEnd -= mainStart;
      mainStart = 0.0f;
    }
    if (mainEnd > mainLength) {
      mainStart = std::max(0.0f, mainStart - (mainEnd - mainLength));
      mainEnd = mainLength;
    }
    float crossStart =
        snapToPixel(style.centered ? (crossLength - crossSize) * 0.5f : crossLength - crossSize, crossScale);
    float crossEnd = crossStart + crossSize;
    if (crossStart < 0.0f) {
      crossEnd -= crossStart;
      crossStart = 0.0f;
    }
    if (crossEnd > crossLength) {
      crossStart = std::max(0.0f, crossStart - (crossEnd - crossLength));
      crossEnd = crossLength;
    }
    const float amount = barCount <= 1 ? 0.0f : static_cast<float>(index) / static_cast<float>(barCount - 1);
    paint.setColor4f(skColor(interpolateColor(style.color1, style.color2, amount)));
    const SkRect rect = horizontal ? SkRect::MakeLTRB(mainStart, crossStart, mainEnd, crossEnd)
                                   : SkRect::MakeLTRB(crossStart, mainStart, crossEnd, mainEnd);
    m_canvas->drawRect(rect, paint);
  }
  endDraw();
}
void GraphiteRenderBackend::drawFancyAudioVisualizer(
    TextureId audioTexture, int textureWidth, float /*surfaceWidth*/, float /*surfaceHeight*/, float width,
    float height, const FancyAudioVisualizerStyle& style, const Mat3& transform
) {
  if (width <= 0.0f || height <= 0.0f || textureWidth <= 0 || !prepareExternalTexture(audioTexture)) {
    return;
  }
  auto* image = m_graphics.textureManager().image(audioTexture);
  if (image == nullptr) {
    return;
  }
  sk_sp<SkShader> audioShader =
      image->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, SkSamplingOptions(SkFilterMode::kNearest));
  if (audioShader == nullptr) {
    throw std::runtime_error("failed to create Graphite fancy visualizer audio shader");
  }

  SkRuntimeShaderBuilder builder(m_runtimeEffects.get(GraphiteRuntimeEffectId::FancyAudioVisualizer));
  builder.child("u_audio_source") = std::move(audioShader);
  builder.uniform("u_texture_width") = static_cast<float>(textureWidth);
  builder.uniform("u_time") = style.time;
  builder.uniform("u_item_width") = width;
  builder.uniform("u_item_height") = height;
  builder.uniform("u_primary_color") = skColor(style.primaryColor);
  builder.uniform("u_secondary_color") = skColor(style.secondaryColor);
  builder.uniform("u_sensitivity") = style.sensitivity;
  builder.uniform("u_rotation_speed") = style.rotationSpeed;
  builder.uniform("u_bar_width") = style.barWidth;
  builder.uniform("u_ring_opacity") = style.ringOpacity;
  builder.uniform("u_corner_radius") = style.cornerRadius;
  builder.uniform("u_bloom_intensity") = style.bloomIntensity;
  builder.uniform("u_mode") = static_cast<float>(style.mode);
  builder.uniform("u_wave_thickness") = style.waveThickness;
  builder.uniform("u_inner_diameter") = style.innerDiameter;
  sk_sp<SkShader> shader = builder.makeShader();
  if (shader == nullptr) {
    throw std::runtime_error("failed to instantiate Graphite fancy audio visualizer shader");
  }

  beginDraw(transform);
  SkPaint paint;
  paint.setAntiAlias(false);
  paint.setBlendMode(blendMode(m_blendMode));
  paint.setShader(std::move(shader));
  m_canvas->drawRect(SkRect::MakeWH(width, height), paint);
  endDraw();
}
void GraphiteRenderBackend::drawEffect(
    float /*surfaceWidth*/, float /*surfaceHeight*/, float width, float height, const EffectStyle& style,
    const Mat3& transform
) {
  if (style.type == EffectType::None || width <= 0.0f || height <= 0.0f) {
    return;
  }

  std::size_t effectIndex = 0;
  bool fog = false;
  switch (style.type) {
  case EffectType::Sky:
    effectIndex = 0;
    break;
  case EffectType::Cloud:
    effectIndex = 1;
    break;
  case EffectType::Fog:
    effectIndex = 1;
    fog = true;
    break;
  case EffectType::Rain:
    effectIndex = 2;
    break;
  case EffectType::Snow:
    effectIndex = 3;
    break;
  case EffectType::Thunder:
    effectIndex = 4;
    break;
  case EffectType::None:
    return;
  }

  const auto weatherId =
      static_cast<GraphiteRuntimeEffectId>(static_cast<std::size_t>(GraphiteRuntimeEffectId::WeatherSky) + effectIndex);
  SkRuntimeShaderBuilder builder(m_runtimeEffects.get(weatherId));
  builder.uniform("u_time") = style.time;
  builder.uniform("u_item_width") = width;
  builder.uniform("u_item_height") = height;
  builder.uniform("u_bg_color") = skColor(style.bgColor);
  builder.uniform("u_radius") = style.radius;
  builder.uniform("u_alternative") = fog ? 1.0f : 0.0f;
  builder.uniform("u_night") = style.night ? 1.0f : 0.0f;
  builder.uniform("u_cloud_amount") = style.cloudAmount;
  builder.uniform("u_intensity") = style.intensity;
  builder.uniform("u_sky_top") = SkV3{style.skyTop.r, style.skyTop.g, style.skyTop.b};
  builder.uniform("u_sky_bottom") = SkV3{style.skyBottom.r, style.skyBottom.g, style.skyBottom.b};
  sk_sp<SkShader> shader = builder.makeShader();
  if (shader == nullptr) {
    throw std::runtime_error("failed to instantiate Graphite weather effect shader");
  }

  beginDraw(transform);
  SkPaint paint;
  paint.setAntiAlias(false);
  paint.setBlendMode(blendMode(m_blendMode));
  paint.setShader(std::move(shader));
  m_canvas->drawRect(SkRect::MakeWH(width, height), paint);
  endDraw();
}
void GraphiteRenderBackend::drawGraph(
    TextureId dataTexture, int textureWidth, float /*surfaceWidth*/, float /*surfaceHeight*/, float width, float height,
    const GraphStyle& style, const Mat3& transform
) {
  if (width <= 0.0f || height <= 0.0f || textureWidth <= 0 || !prepareExternalTexture(dataTexture)) {
    return;
  }
  auto* image = m_graphics.textureManager().image(dataTexture);
  if (image == nullptr) {
    return;
  }
  sk_sp<SkShader> dataShader =
      image->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, SkSamplingOptions(SkFilterMode::kNearest));
  if (dataShader == nullptr) {
    throw std::runtime_error("failed to create Graphite graph data shader");
  }

  SkRuntimeShaderBuilder builder(m_runtimeEffects.get(GraphiteRuntimeEffectId::Graph));
  builder.child("u_data_source") = std::move(dataShader);
  builder.uniform("u_line_color1") = skColor(style.lineColor1);
  builder.uniform("u_line_color2") = skColor(style.lineColor2);
  builder.uniform("u_line_color3") = skColor(style.lineColor3);
  builder.uniform("u_count1") = style.count1;
  builder.uniform("u_count2") = style.count2;
  builder.uniform("u_count3") = style.count3;
  builder.uniform("u_scroll1") = style.scroll1;
  builder.uniform("u_scroll2") = style.scroll2;
  builder.uniform("u_scroll3") = style.scroll3;
  builder.uniform("u_line_width") = style.lineWidth;
  builder.uniform("u_graph_fill_opacity") = style.graphFillOpacity;
  builder.uniform("u_tex_width") = static_cast<float>(textureWidth);
  builder.uniform("u_res_x") = width;
  builder.uniform("u_res_y") = height;
  builder.uniform("u_aa_size") = style.aaSize;
  sk_sp<SkShader> shader = builder.makeShader();
  if (shader == nullptr) {
    throw std::runtime_error("failed to instantiate Graphite graph shader");
  }

  beginDraw(transform);
  SkPaint paint;
  paint.setAntiAlias(false);
  paint.setBlendMode(blendMode(m_blendMode));
  paint.setShader(std::move(shader));
  m_canvas->drawRect(SkRect::MakeWH(width, height), paint);
  endDraw();
}
void GraphiteRenderBackend::drawWallpaper(const WallpaperDrawParams& params) {
  const std::size_t effectIndex = static_cast<std::size_t>(params.transition);
  constexpr std::size_t kWallpaperEffectCount = 6;
  if (effectIndex >= kWallpaperEffectCount || params.quadWidth <= 0.0f || params.quadHeight <= 0.0f) {
    return;
  }

  const auto sourceShader = [this](const WallpaperLayer& layer) -> sk_sp<SkShader> {
    if (layer.kind == WallpaperSourceKind::Color) {
      return SkShaders::Color(skColor(layer.color), nullptr);
    }
    if (!prepareExternalTexture(layer.texture)) {
      return nullptr;
    }
    auto* image = m_graphics.textureManager().image(layer.texture);
    return image == nullptr
        ? nullptr
        : image->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, imageSampling(*image));
  };

  sk_sp<SkShader> fromShader = sourceShader(params.from);
  sk_sp<SkShader> toShader = sourceShader(params.to);
  if (fromShader == nullptr || toShader == nullptr) {
    return;
  }

  const auto wallpaperId = static_cast<GraphiteRuntimeEffectId>(
      static_cast<std::size_t>(GraphiteRuntimeEffectId::WallpaperFade) + effectIndex
  );
  SkRuntimeShaderBuilder builder(m_runtimeEffects.get(wallpaperId));
  builder.child("u_source1") = std::move(fromShader);
  builder.child("u_source2") = std::move(toShader);
  builder.uniform("u_sourceKind1") = params.from.kind == WallpaperSourceKind::Color ? 1.0f : 0.0f;
  builder.uniform("u_sourceKind2") = params.to.kind == WallpaperSourceKind::Color ? 1.0f : 0.0f;
  builder.uniform("u_sourceColor1") = skColor(params.from.color);
  builder.uniform("u_sourceColor2") = skColor(params.to.color);
  builder.uniform("u_progress") = params.progress;
  builder.uniform("u_fillMode") = params.fillMode;
  builder.uniform("u_imageWidth1") = params.from.imageWidth;
  builder.uniform("u_imageHeight1") = params.from.imageHeight;
  builder.uniform("u_imageWidth2") = params.to.imageWidth;
  builder.uniform("u_imageHeight2") = params.to.imageHeight;
  builder.uniform("u_screenWidth") = params.quadWidth;
  builder.uniform("u_screenHeight") = params.quadHeight;
  builder.uniform("u_fillColor") = skColor(params.fillColor);
  builder.uniform("u_spanOffset") = SkV2{params.span.offsetX, params.span.offsetY};
  builder.uniform("u_spanMonitorSize") = SkV2{params.span.monitorWidth, params.span.monitorHeight};
  builder.uniform("u_spanTotalSize") = SkV2{params.span.totalWidth, params.span.totalHeight};

  const TransitionParams& transition = params.params;
  switch (effectIndex) {
  case 1:
    builder.uniform("u_direction") = transition.direction;
    builder.uniform("u_smoothness") = transition.smoothness;
    break;
  case 2:
    builder.uniform("u_smoothness") = transition.smoothness;
    builder.uniform("u_centerX") = transition.centerX;
    builder.uniform("u_centerY") = transition.centerY;
    builder.uniform("u_aspectRatio") = transition.aspectRatio;
    break;
  case 3:
    builder.uniform("u_smoothness") = transition.smoothness;
    builder.uniform("u_aspectRatio") = transition.aspectRatio;
    builder.uniform("u_stripeCount") = transition.stripeCount;
    builder.uniform("u_angle") = transition.angle;
    break;
  case 5:
    builder.uniform("u_cellSize") = transition.cellSize;
    builder.uniform("u_centerX") = transition.centerX;
    builder.uniform("u_centerY") = transition.centerY;
    builder.uniform("u_aspectRatio") = transition.aspectRatio;
    builder.uniform("u_smoothness") = transition.smoothness;
    break;
  default:
    break;
  }

  sk_sp<SkShader> shader = builder.makeShader();
  if (shader == nullptr) {
    throw std::runtime_error("failed to instantiate Graphite wallpaper transition shader");
  }
  beginDraw(params.transform);
  SkPaint paint;
  paint.setAntiAlias(false);
  paint.setBlendMode(blendMode(m_blendMode));
  paint.setShader(std::move(shader));
  m_canvas->drawRect(SkRect::MakeWH(params.quadWidth, params.quadHeight), paint);
  endDraw();
}

void GraphiteRenderBackend::drawFullscreenTexture(TextureId texture) {
  if (!prepareExternalTexture(texture)) {
    return;
  }
  auto* image = m_graphics.textureManager().image(texture);
  if (m_canvas == nullptr || image == nullptr || m_target == nullptr) {
    return;
  }
  m_canvas->save();
  m_canvas->drawImageRect(
      image,
      SkRect::MakeWH(static_cast<float>(m_target->logicalWidth()), static_cast<float>(m_target->logicalHeight())),
      imageSampling(*image), nullptr
  );
  m_canvas->restore();
}

void GraphiteRenderBackend::drawFullscreenTint(Color color) {
  if (m_canvas == nullptr) {
    return;
  }
  float width = 0.0f;
  float height = 0.0f;
  if (m_boundFramebuffer != nullptr) {
    width = static_cast<float>(m_boundFramebuffer->width());
    height = static_cast<float>(m_boundFramebuffer->height());
  } else if (m_target != nullptr) {
    width = static_cast<float>(m_target->logicalWidth());
    height = static_cast<float>(m_target->logicalHeight());
  }
  if (width <= 0.0f || height <= 0.0f) {
    return;
  }
  SkPaint paint;
  paint.setColor4f(skColor(color));
  paint.setBlendMode(blendMode(m_blendMode));
  m_canvas->drawRect(SkRect::MakeWH(width, height), paint);
}
void GraphiteRenderBackend::drawFramebufferBlur(
    TextureId sourceTexture, std::uint32_t width, std::uint32_t height, float directionX, float directionY, float radius
) {
  if (m_canvas == nullptr
      || sourceTexture == TextureId{}
      || width == 0
      || height == 0
      || radius < 0.0f
      || !prepareExternalTexture(sourceTexture)) {
    return;
  }
  auto* image = m_graphics.textureManager().image(sourceTexture);
  if (image == nullptr) {
    return;
  }

  const float sigma = radius * 0.5f;
  SkPaint paint;
  paint.setAntiAlias(false);
  paint.setBlendMode(blendMode(m_blendMode));
  paint.setImageFilter(
      SkImageFilters::Blur(std::abs(directionX) * sigma, std::abs(directionY) * sigma, SkTileMode::kClamp, nullptr)
  );
  const SkRect bounds = SkRect::MakeWH(static_cast<float>(width), static_cast<float>(height));
  m_canvas->drawImageRect(
      image, bounds, bounds, SkSamplingOptions(SkFilterMode::kLinear), &paint, SkCanvas::kStrict_SrcRectConstraint
  );
}

TextureManager& GraphiteRenderBackend::textureManager() { return m_graphics.textureManager(); }

bool GraphiteRenderBackend::prepareExternalTexture(TextureId texture) {
  auto* synchronization = m_graphics.textureManager().externalSynchronization(texture);
  if (synchronization == nullptr
      || std::ranges::any_of(m_externalSubmissions, [synchronization](const ExternalSubmission& submission) {
           return submission.synchronization == synchronization;
         })) {
    return true;
  }
  GraphiteSubmissionDependency dependency;
  if (!synchronization->prepareForGraphiteSampling(dependency)) {
    return false;
  }
  m_externalSubmissions.push_back({
      .synchronization = synchronization,
      .dependency = dependency,
  });
  return true;
}

void GraphiteRenderBackend::finishExternalTextures(bool submitted) {
  for (const auto& submission : m_externalSubmissions) {
    submission.synchronization->finishGraphiteSampling(submission.dependency, submitted);
  }
  m_externalSubmissions.clear();
}

std::unique_ptr<RenderBackend> createGraphiteRenderBackend(GraphicsDevice& graphics) {
  return std::make_unique<GraphiteRenderBackend>(graphics);
}
