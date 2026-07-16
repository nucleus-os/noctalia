#include "render/backend/graphite_offscreen_golden.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkM44.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkShader.h"
#include "include/core/SkSurface.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/GraphiteTypes.h"
#include "include/gpu/graphite/Recorder.h"
#include "render/backend/graphite_framebuffer.h"
#include "render/backend/graphite_render_backend.h"
#include "config/config_types.h"
#include "render/core/blur_cache.h"
#include "render/core/image_file_loader.h"
#include "render/core/render_styles.h"
#include "render/core/wallpaper_types.h"
#include "render/graphics_device.h"
#include "render/text/skparagraph_text_renderer.h"

#include <nucleus/text/TextLayoutBuilder.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <memory>
#include <sstream>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
  constexpr int kSize = 32;

  struct Readback {
    bool called = false;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
  };

  void readbackCallback(void* context, std::unique_ptr<const SkImage::AsyncReadResult> result) {
    auto& readback = *static_cast<Readback*>(context);
    readback.called = true;
    if (result == nullptr || result->count() != 1 || result->data(0) == nullptr) {
      return;
    }
    readback.pixels.resize(static_cast<std::size_t>(readback.width * readback.height * 4));
    const auto* source = static_cast<const std::uint8_t*>(result->data(0));
    for (int y = 0; y < readback.height; ++y) {
      std::memcpy(
          readback.pixels.data() + static_cast<std::size_t>(y * readback.width * 4),
          source + static_cast<std::size_t>(y) * result->rowBytes(0),
          static_cast<std::size_t>(readback.width * 4)
      );
    }
  }

  std::vector<std::uint8_t> readPixels(GraphicsDevice& graphics, const GraphiteFramebuffer& framebuffer) {
    auto drawRecording = graphics.recorder()->snap();
    if (drawRecording == nullptr) {
      throw std::runtime_error("Graphite golden could not snap its drawing");
    }
    skgpu::graphite::InsertRecordingInfo drawInsertInfo;
    drawInsertInfo.fRecording = drawRecording.get();
    if (!graphics.graphiteContext()->insertRecording(drawInsertInfo)
        || !graphics.graphiteContext()->submit(skgpu::graphite::SyncToCpu::kYes)) {
      throw std::runtime_error("Graphite golden could not submit its drawing");
    }

    const int width = static_cast<int>(framebuffer.width());
    const int height = static_cast<int>(framebuffer.height());
    Readback readback{.width = width, .height = height};
    const SkImageInfo info = SkImageInfo::Make(
        width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB()
    );
    graphics.graphiteContext()->asyncRescaleAndReadPixels(
        framebuffer.readbackSurfaceForTesting(), info, SkIRect::MakeWH(width, height), SkImage::RescaleGamma::kSrc,
        SkImage::RescaleMode::kNearest, readbackCallback, &readback
    );
    auto readRecording = graphics.recorder()->snap();
    if (readRecording == nullptr) {
      throw std::runtime_error("Graphite golden could not snap its recording");
    }
    skgpu::graphite::InsertRecordingInfo insertInfo;
    insertInfo.fRecording = readRecording.get();
    if (!graphics.graphiteContext()->insertRecording(insertInfo)
        || !graphics.graphiteContext()->submit(skgpu::graphite::SyncToCpu::kYes)) {
      throw std::runtime_error("Graphite golden could not submit its readback");
    }
    graphics.graphiteContext()->checkAsyncWorkCompletion();
    if (!readback.called || readback.pixels.empty()) {
      throw std::runtime_error("Graphite golden readback did not complete");
    }
    return std::move(readback.pixels);
  }

  const std::uint8_t* pixel(const std::vector<std::uint8_t>& pixels, int width, int x, int y) {
    return pixels.data() + static_cast<std::size_t>((y * width + x) * 4);
  }

  bool near(std::uint8_t value, int expected, int tolerance = 18) {
    return value >= expected - tolerance && value <= expected + tolerance;
  }

  struct ParagraphHandle {
    nucleus::text::TextLayoutService service;
    std::uint64_t value = 0;
    ~ParagraphHandle() { if (value != 0) service.release(value); }
    ParagraphHandle() = default;
    ParagraphHandle(const ParagraphHandle&) = delete;
    ParagraphHandle& operator=(const ParagraphHandle&) = delete;
    ParagraphHandle(ParagraphHandle&& other) noexcept : value(std::exchange(other.value, 0)) {}
    ParagraphHandle& operator=(ParagraphHandle&&) = delete;
  };

  ParagraphHandle makeParagraph(
      const nucleus::text::TextRunView* runs, std::size_t count, float width,
      nucleus::text::ParagraphDirection direction = nucleus::text::ParagraphDirection::Automatic,
      std::uint32_t maximumLines = 0,
      nucleus::text::EllipsisMode ellipsisMode = nucleus::text::EllipsisMode::None
  ) {
    ParagraphHandle result;
    const nucleus::text::ParagraphStyle style{
        .width = width, .maximumNumberOfLines = maximumLines, .ellipsisMode = ellipsisMode,
        .direction = direction,
    };
    nucleus::text::ParagraphMetrics metrics{};
    if (!result.service.createRuns(runs, count, &style, &result.value, &metrics) || result.value == 0) {
      throw std::runtime_error("Graphite text golden could not create a paragraph");
    }
    return result;
  }

  struct RegionColors {
    bool red = false;
    bool green = false;
    bool blue = false;
    bool chromatic = false;
    bool ink = false;
  };

  RegionColors scanRegion(
      const std::vector<std::uint8_t>& pixels, int width, int x0, int y0, int x1, int y1
  ) {
    RegionColors result;
    for (int y = y0; y < y1; ++y) {
      for (int x = x0; x < x1; ++x) {
        const auto* rgba8 = pixel(pixels, width, x, y);
        if (rgba8[3] == 0) continue;
        result.ink = true;
        result.red |= rgba8[0] > rgba8[1] * 2 && rgba8[0] > rgba8[2] * 2;
        result.green |= rgba8[1] > rgba8[0] * 2 && rgba8[1] > rgba8[2] * 2;
        result.blue |= rgba8[2] > rgba8[0] * 2 && rgba8[2] > rgba8[1] * 2;
        const int minimum = std::min({rgba8[0], rgba8[1], rgba8[2]});
        const int maximum = std::max({rgba8[0], rgba8[1], rgba8[2]});
        result.chromatic |= maximum - minimum > 24;
      }
    }
    return result;
  }

  int maximumDominantRedPixelsInRow(
      const std::vector<std::uint8_t>& pixels, int width, int x0, int y0, int x1, int y1
  ) {
    int maximum = 0;
    for (int y = y0; y < y1; ++y) {
      int count = 0;
      for (int x = x0; x < x1; ++x) {
        const auto* rgba8 = pixel(pixels, width, x, y);
        count += rgba8[3] > 0 && rgba8[0] > rgba8[1] * 2 && rgba8[0] > rgba8[2] * 2 ? 1 : 0;
      }
      maximum = std::max(maximum, count);
    }
    return maximum;
  }

  void setFloatIfPresent(SkRuntimeShaderBuilder& builder, std::string_view name, float value) {
    if (builder.effect()->findUniform(name) != nullptr) builder.uniform(name) = value;
  }

  std::vector<std::uint8_t> copyPremultipliedRgba(const SkPixmap& pixmap, int width, int height) {
    std::vector<std::uint8_t> result(static_cast<std::size_t>(width * height * 4));
    const auto* source = static_cast<const std::uint8_t*>(pixmap.addr());
    for (int y = 0; y < height; ++y) {
      std::memcpy(
          result.data() + static_cast<std::size_t>(y * width * 4),
          source + static_cast<std::size_t>(y) * pixmap.rowBytes(), static_cast<std::size_t>(width * 4)
      );
    }
    return result;
  }

  std::vector<std::uint8_t> renderWallpaperCpuReference(
      const sk_sp<SkRuntimeEffect>& effect, int size, float progress
  ) {
    SkRuntimeShaderBuilder builder(effect);
    builder.child("u_source1") = SkShaders::Color(SK_ColorTRANSPARENT);
    builder.child("u_source2") = SkShaders::Color(SK_ColorTRANSPARENT);
    builder.uniform("u_sourceKind1") = 1.0f;
    builder.uniform("u_sourceKind2") = 1.0f;
    builder.uniform("u_sourceColor1") = SkV4{1.0f, 0.0f, 0.0f, 1.0f};
    builder.uniform("u_sourceColor2") = SkV4{0.0f, 1.0f, 0.0f, 1.0f};
    builder.uniform("u_progress") = progress;
    builder.uniform("u_fillMode") = 0.0f;
    builder.uniform("u_imageWidth1") = static_cast<float>(size);
    builder.uniform("u_imageHeight1") = static_cast<float>(size);
    builder.uniform("u_imageWidth2") = static_cast<float>(size);
    builder.uniform("u_imageHeight2") = static_cast<float>(size);
    builder.uniform("u_screenWidth") = static_cast<float>(size);
    builder.uniform("u_screenHeight") = static_cast<float>(size);
    builder.uniform("u_fillColor") = SkV4{0.0f, 0.0f, 0.0f, 1.0f};
    builder.uniform("u_spanOffset") = SkV2{0.0f, 0.0f};
    builder.uniform("u_spanMonitorSize") = SkV2{static_cast<float>(size), static_cast<float>(size)};
    builder.uniform("u_spanTotalSize") = SkV2{static_cast<float>(size), static_cast<float>(size)};
    setFloatIfPresent(builder, "u_direction", 0.0f);
    setFloatIfPresent(builder, "u_smoothness", 0.25f);
    setFloatIfPresent(builder, "u_centerX", 0.5f);
    setFloatIfPresent(builder, "u_centerY", 0.5f);
    setFloatIfPresent(builder, "u_aspectRatio", 1.0f);
    setFloatIfPresent(builder, "u_stripeCount", 12.0f);
    setFloatIfPresent(builder, "u_angle", 30.0f);
    setFloatIfPresent(builder, "u_cellSize", 0.04f);
    sk_sp<SkShader> shader = builder.makeShader();
    auto surface = SkSurfaces::Raster(
        SkImageInfo::Make(size, size, kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB())
    );
    if (shader == nullptr || surface == nullptr) {
      throw std::runtime_error("wallpaper CPU reference could not instantiate its shader");
    }
    SkPaint paint;
    paint.setShader(std::move(shader));
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    surface->getCanvas()->drawPaint(paint);
    SkPixmap pixmap;
    if (!surface->peekPixels(&pixmap)) {
      throw std::runtime_error("wallpaper CPU reference could not read its pixels");
    }
    return copyPremultipliedRgba(pixmap, size, size);
  }

  std::vector<std::uint8_t> renderWeatherCpuReference(
      const sk_sp<SkRuntimeEffect>& effect, int size, bool fog
  ) {
    SkRuntimeShaderBuilder builder(effect);
    builder.uniform("u_time") = 7.25f;
    builder.uniform("u_item_width") = static_cast<float>(size);
    builder.uniform("u_item_height") = static_cast<float>(size);
    builder.uniform("u_bg_color") = SkV4{0.0f, 0.0f, 0.0f, 1.0f};
    builder.uniform("u_radius") = 6.0f;
    builder.uniform("u_alternative") = fog ? 1.0f : 0.0f;
    builder.uniform("u_night") = 0.0f;
    builder.uniform("u_cloud_amount") = 0.5f;
    builder.uniform("u_intensity") = 1.0f;
    builder.uniform("u_sky_top") = SkV3{0.25f, 0.65f, 1.0f};
    builder.uniform("u_sky_bottom") = SkV3{0.02f, 0.12f, 0.35f};
    sk_sp<SkShader> shader = builder.makeShader();
    auto surface = SkSurfaces::Raster(
        SkImageInfo::Make(size, size, kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB())
    );
    if (shader == nullptr || surface == nullptr) {
      throw std::runtime_error("weather CPU reference could not instantiate its shader");
    }
    SkPaint paint;
    paint.setShader(std::move(shader));
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    surface->getCanvas()->drawPaint(paint);
    SkPixmap pixmap;
    if (!surface->peekPixels(&pixmap)) {
      throw std::runtime_error("weather CPU reference could not read its pixels");
    }
    return copyPremultipliedRgba(pixmap, size, size);
  }

  std::vector<std::uint8_t> renderDataDrivenCpuReference(
      const sk_sp<SkRuntimeEffect>& effect, std::span<const std::uint8_t> data,
      int dataWidth, int size, bool visualizer
  ) {
    const SkImageInfo dataInfo = SkImageInfo::Make(
        dataWidth, 1, kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB()
    );
    const SkPixmap dataPixmap(dataInfo, data.data(), dataInfo.minRowBytes());
    sk_sp<SkImage> dataImage = SkImages::RasterFromPixmapCopy(dataPixmap);
    if (dataImage == nullptr) throw std::runtime_error("data-driven CPU reference image creation failed");
    sk_sp<SkShader> dataShader = dataImage->makeShader(
        SkTileMode::kClamp, SkTileMode::kClamp, SkSamplingOptions(SkFilterMode::kNearest)
    );
    SkRuntimeShaderBuilder builder(effect);
    if (visualizer) {
      builder.child("u_audio_source") = std::move(dataShader);
      builder.uniform("u_texture_width") = static_cast<float>(dataWidth);
      builder.uniform("u_time") = 3.5f;
      builder.uniform("u_item_width") = static_cast<float>(size);
      builder.uniform("u_item_height") = static_cast<float>(size);
      builder.uniform("u_primary_color") = SkV4{1.0f, 0.1f, 0.2f, 1.0f};
      builder.uniform("u_secondary_color") = SkV4{0.1f, 0.5f, 1.0f, 1.0f};
      builder.uniform("u_sensitivity") = 1.0f;
      builder.uniform("u_rotation_speed") = 0.5f;
      builder.uniform("u_bar_width") = 0.6f;
      builder.uniform("u_ring_opacity") = 0.8f;
      builder.uniform("u_corner_radius") = 12.0f;
      builder.uniform("u_bloom_intensity") = 0.5f;
      builder.uniform("u_mode") = static_cast<float>(FancyAudioVisualizerMode::All);
      builder.uniform("u_wave_thickness") = 1.0f;
      builder.uniform("u_inner_diameter") = 0.7f;
    } else {
      builder.child("u_data_source") = std::move(dataShader);
      builder.uniform("u_line_color1") = SkV4{1.0f, 0.0f, 0.0f, 1.0f};
      builder.uniform("u_line_color2") = SkV4{0.0f, 1.0f, 0.0f, 1.0f};
      builder.uniform("u_line_color3") = SkV4{0.0f, 0.0f, 1.0f, 1.0f};
      builder.uniform("u_count1") = 28.0f;
      builder.uniform("u_count2") = 28.0f;
      builder.uniform("u_count3") = 28.0f;
      builder.uniform("u_scroll1") = 1.0f;
      builder.uniform("u_scroll2") = 1.0f;
      builder.uniform("u_scroll3") = 1.0f;
      builder.uniform("u_line_width") = 2.0f;
      builder.uniform("u_graph_fill_opacity") = 0.2f;
      builder.uniform("u_tex_width") = static_cast<float>(dataWidth);
      builder.uniform("u_res_x") = static_cast<float>(size);
      builder.uniform("u_res_y") = static_cast<float>(size);
      builder.uniform("u_aa_size") = 0.5f;
    }
    sk_sp<SkShader> shader = builder.makeShader();
    auto surface = SkSurfaces::Raster(
        SkImageInfo::Make(size, size, kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB())
    );
    if (shader == nullptr || surface == nullptr) {
      throw std::runtime_error("data-driven CPU reference shader creation failed");
    }
    SkPaint paint;
    paint.setShader(std::move(shader));
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    surface->getCanvas()->drawPaint(paint);
    SkPixmap pixmap;
    if (!surface->peekPixels(&pixmap)) throw std::runtime_error("data-driven CPU reference readback failed");
    return copyPremultipliedRgba(pixmap, size, size);
  }

  void requireCpuGpuReferenceMatch(
      const std::vector<std::uint8_t>& gpu, int gpuWidth, int tileX, int tileY,
      const std::vector<std::uint8_t>& cpu, int size, int effectIndex,
      double maximumMeanError = 4.0, double maximumDivergentFraction = 0.08
  ) {
    std::uint64_t absoluteError = 0;
    std::uint64_t comparedChannels = 0;
    std::uint64_t divergentPixels = 0;
    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        const auto* actual = pixel(gpu, gpuWidth, tileX + x, tileY + y);
        const auto* expected = pixel(cpu, size, x, y);
        bool divergent = false;
        for (int channel = 0; channel < 4; ++channel) {
          const int difference = std::abs(static_cast<int>(actual[channel]) - static_cast<int>(expected[channel]));
          absoluteError += static_cast<std::uint64_t>(difference);
          ++comparedChannels;
          divergent |= difference > 24;
        }
        divergentPixels += divergent ? 1U : 0U;
      }
    }
    const double meanError = static_cast<double>(absoluteError) / static_cast<double>(comparedChannels);
    const double divergentFraction =
        static_cast<double>(divergentPixels) / static_cast<double>(size * size);
    if (meanError > maximumMeanError || divergentFraction > maximumDivergentFraction) {
      std::ostringstream message;
      message << "Graphite CPU/GPU reference drifted for effect " << effectIndex
              << ": mean-channel-error=" << meanError << " divergent-pixels=" << divergentFraction;
      throw std::runtime_error(message.str());
    }
  }
} // namespace

void runGraphiteOffscreenGolden(GraphicsDevice& graphics) {
  GraphiteRenderBackend backend(graphics);
  auto sourceBase = backend.createFramebuffer(kSize, kSize);
  auto scratchBase = backend.createFramebuffer(kSize, kSize);
  auto finalBase = backend.createFramebuffer(kSize, kSize);
  auto* source = dynamic_cast<GraphiteFramebuffer*>(sourceBase.get());
  auto* scratch = dynamic_cast<GraphiteFramebuffer*>(scratchBase.get());
  auto* final = dynamic_cast<GraphiteFramebuffer*>(finalBase.get());
  if (source == nullptr || scratch == nullptr || final == nullptr) {
    throw std::runtime_error("Graphite golden could not create offscreen surfaces");
  }

  backend.beginOffscreenFrame(*source);
  backend.clear(rgba(0.0f, 0.0f, 0.0f, 0.0f));
  RoundedRectStyle red;
  red.fill = rgba(1.0f, 0.0f, 0.0f, 1.0f);
  backend.drawRect(kSize, kSize, 12.0f, 12.0f, red, Mat3::translation(10.0f, 10.0f));
  RoundedRectStyle green;
  green.fill = rgba(0.0f, 1.0f, 0.0f, 1.0f);
  backend.drawRect(kSize, kSize, 4.0f, 4.0f, green, Mat3::translation(1.0f, 2.0f));
  backend.endOffscreenFrame();
  const auto sourcePixels = readPixels(graphics, *source);

  BlurCache productionBlurCache;
  const TextureHandle cachedSource = productionBlurCache.get(
      backend, TextureHandle{source->colorTexture(), kSize, kSize}, kSize, kSize, 0.0f, 0
  );
  if (!cachedSource.valid()) throw std::runtime_error("Graphite production BlurCache probe failed to cache source");
  auto cacheProbeBase = backend.createFramebuffer(kSize, kSize);
  auto* cacheProbe = dynamic_cast<GraphiteFramebuffer*>(cacheProbeBase.get());
  if (cacheProbe == nullptr) throw std::runtime_error("Graphite production BlurCache probe surface failed");
  backend.beginOffscreenFrame(*cacheProbe);
  backend.clear(rgba(0.0f, 0.0f, 0.0f, 0.0f));
  backend.drawImage(RenderImageDraw{
      .texture = cachedSource.id,
      .surfaceWidth = static_cast<float>(kSize),
      .surfaceHeight = static_cast<float>(kSize),
      .width = static_cast<float>(kSize),
      .height = static_cast<float>(kSize),
      .textureWidth = static_cast<float>(kSize),
      .textureHeight = static_cast<float>(kSize),
  });
  backend.endOffscreenFrame();
  const auto cacheProbePixels = readPixels(graphics, *cacheProbe);
  const auto* cachedTopMarker = pixel(cacheProbePixels, kSize, 2, 3);
  const auto* cachedMirroredMarker = pixel(cacheProbePixels, kSize, 2, kSize - 4);
  if (cachedTopMarker[1] < 220 || cachedTopMarker[3] < 220 || cachedMirroredMarker[3] > 10) {
    throw std::runtime_error("Graphite production BlurCache inverted its top-left source orientation");
  }
  productionBlurCache.destroy();
  backend.setBlendMode(RenderBlendMode::PremultipliedAlpha);

  backend.beginOffscreenFrame(*scratch);
  backend.clear(rgba(0.0f, 0.0f, 0.0f, 0.0f));
  backend.drawFramebufferBlur(source->colorTexture(), kSize, kSize, 1.0f, 0.0f, 6.0f);
  backend.endOffscreenFrame();
  backend.beginOffscreenFrame(*final);
  backend.clear(rgba(0.0f, 0.0f, 0.0f, 0.0f));
  backend.drawFramebufferBlur(scratch->colorTexture(), kSize, kSize, 0.0f, 1.0f, 6.0f);
  backend.drawFullscreenTint(rgba(0.0f, 0.0f, 1.0f, 0.25f));

  backend.endOffscreenFrame();
  const auto finalPixels = readPixels(graphics, *final);
  const auto* topMarker = pixel(sourcePixels, kSize, 2, 3);
  const auto* mirroredMarker = pixel(sourcePixels, kSize, 2, kSize - 4);
  const auto* sourceCenter = pixel(sourcePixels, kSize, 16, 16);
  const auto* sourceCorner = pixel(sourcePixels, kSize, 31, 31);
  const auto* blurredEdge = pixel(finalPixels, kSize, 8, 16);
  const auto* finalCenter = pixel(finalPixels, kSize, 16, 16);
  if (topMarker[1] < 220 || topMarker[3] < 220 || mirroredMarker[3] > 10
      || sourceCenter[0] < 240 || sourceCenter[1] > 10 || sourceCenter[3] < 240
      || sourceCorner[3] > 10 || blurredEdge[0] == 0 || blurredEdge[3] <= 64
      || !near(finalCenter[0], 191, 35) || finalCenter[2] < 55 || finalCenter[3] < 225) {
    std::ostringstream message;
    message << "Graphite offscreen golden pixel assertions failed: top="
            << +topMarker[0] << ',' << +topMarker[1] << ',' << +topMarker[2] << ',' << +topMarker[3]
            << " mirroredA=" << +mirroredMarker[3]
            << " sourceCenter=" << +sourceCenter[0] << ',' << +sourceCenter[1] << ',' << +sourceCenter[2] << ','
            << +sourceCenter[3] << " cornerA=" << +sourceCorner[3]
            << " blurredEdge=" << +blurredEdge[0] << ',' << +blurredEdge[1] << ',' << +blurredEdge[2] << ','
            << +blurredEdge[3] << " finalCenter=" << +finalCenter[0] << ',' << +finalCenter[1] << ','
            << +finalCenter[2] << ',' << +finalCenter[3];
    throw std::runtime_error(message.str());
  }

  constexpr int kTextWidth = 384;
  constexpr int kTextHeight = 196;
  auto textBase = backend.createFramebuffer(kTextWidth, kTextHeight);
  auto* textTarget = dynamic_cast<GraphiteFramebuffer*>(textBase.get());
  if (textTarget == nullptr) throw std::runtime_error("Graphite text golden could not create its surface");

  const std::string latin = "Graphite underline";
  const nucleus::text::TextRunView latinRun{
      .text = {latin.data(), latin.size()}, .pointSize = 28.0f, .weight = nucleus::text::FontWeightBold,
      .underline = true, .strikeThrough = true,
      .red = 1.0f, .green = 0.0f, .blue = 0.0f, .alpha = 1.0f,
  };
  const std::string arabic = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85";
  constexpr std::string_view arabicFamily = "Noto Sans Arabic";
  const nucleus::text::TextRunView arabicRun{
      .text = {arabic.data(), arabic.size()}, .fontFamily = {arabicFamily.data(), arabicFamily.size()},
      .pointSize = 28.0f, .red = 0.0f, .green = 1.0f, .blue = 0.0f, .alpha = 1.0f,
  };
  const std::string cjk = "\xE5\x9B\xBE\xE5\xBD\xA2\xE6\xB8\xB2\xE6\x9F\x93";
  constexpr std::string_view cjkFamily = "Noto Sans CJK SC";
  const nucleus::text::TextRunView cjkRun{
      .text = {cjk.data(), cjk.size()}, .fontFamily = {cjkFamily.data(), cjkFamily.size()},
      .pointSize = 28.0f, .red = 0.0f, .green = 0.0f, .blue = 1.0f, .alpha = 1.0f,
  };
  const std::string emoji = "\xF0\x9F\x8C\x88";
  constexpr std::string_view emojiFamily = "Noto Color Emoji";
  const nucleus::text::TextRunView emojiRun{
      .text = {emoji.data(), emoji.size()}, .fontFamily = {emojiFamily.data(), emojiFamily.size()}, .pointSize = 42.0f,
  };
  auto latinParagraph = makeParagraph(&latinRun, 1, kTextWidth);
  auto arabicParagraph = makeParagraph(
      &arabicRun, 1, kTextWidth, nucleus::text::ParagraphDirection::Rtl
  );
  auto cjkParagraph = makeParagraph(&cjkRun, 1, kTextWidth);
  auto emojiParagraph = makeParagraph(&emojiRun, 1, 64.0f);
  const std::string ellipsisPrefix = "styled prefix that must remain ";
  const std::string ellipsisSuffix = "suffix must remain";
  const std::vector<StyledTextRun> ellipsisRuns{
      {.text = ellipsisPrefix, .color = rgba(1.0f, 0.0f, 0.0f, 1.0f)},
      {.text = ellipsisSuffix, .color = rgba(0.0f, 0.0f, 1.0f, 1.0f)},
  };
  SkParagraphTextRenderer productionText;
  productionText.initialize(&backend);
  const auto ellipsisMetrics = productionText.measureStyled(
      ellipsisRuns, 20.0f, FontWeight::Normal, 220.0f, 1, TextAlign::Start, {}, TextEllipsize::Middle,
      ParagraphDirection::Ltr);

  backend.beginOffscreenFrame(*textTarget);
  backend.clear(rgba(0.0f, 0.0f, 0.0f, 0.0f));
  backend.setBlendMode(RenderBlendMode::PremultipliedAlpha);
  backend.drawParagraph(latinParagraph.value, 4.0f, 0.0f, Mat3::identity());
  backend.drawParagraph(arabicParagraph.value, 0.0f, 36.0f, Mat3::identity());
  backend.drawParagraph(cjkParagraph.value, 4.0f, 74.0f, Mat3::identity());
  backend.drawParagraph(emojiParagraph.value, 300.0f, 100.0f, Mat3::identity());
  productionText.drawStyled(
      kTextWidth, kTextHeight, 4.0f, 154.0f - ellipsisMetrics.top, ellipsisRuns, 20.0f,
      rgba(1.0f, 1.0f, 1.0f, 1.0f), Mat3::identity(), FontWeight::Normal, 220.0f, 1,
      TextAlign::Start, {}, TextEllipsize::Middle, ParagraphDirection::Ltr);
  backend.endOffscreenFrame();
  const auto textPixels = readPixels(graphics, *textTarget);
  const RegionColors latinColors = scanRegion(textPixels, kTextWidth, 0, 0, kTextWidth, 36);
  const RegionColors arabicLeft = scanRegion(textPixels, kTextWidth, 0, 36, kTextWidth / 2, 74);
  const RegionColors arabicRight = scanRegion(textPixels, kTextWidth, kTextWidth / 2, 36, kTextWidth, 74);
  const RegionColors cjkColors = scanRegion(textPixels, kTextWidth, 0, 74, 220, 112);
  const RegionColors emojiColors = scanRegion(textPixels, kTextWidth, 290, 96, 370, kTextHeight);
  const RegionColors ellipsisColors = scanRegion(textPixels, kTextWidth, 0, 150, 230, kTextHeight);
  const auto* transparent = pixel(textPixels, kTextWidth, 270, 180);
  const int underlineRun = maximumDominantRedPixelsInRow(textPixels, kTextWidth, 0, 0, 300, 36);
  if (!latinColors.red || underlineRun < 100
      || arabicLeft.green || !arabicRight.green || !cjkColors.blue
      || !emojiColors.ink || !emojiColors.chromatic
      || !ellipsisColors.red || !ellipsisColors.blue || transparent[3] != 0) {
    throw std::runtime_error("Graphite multilingual/color-font text golden pixel assertions failed");
  }
  productionText.cleanup();

  // Exercise the geometry consumed by multiline Input selection and bidi
  // caret affinity, then paint those real selection boxes through the same
  // rectangle and paragraph paths as the production control.
  constexpr int kSelectionWidth = 180;
  constexpr int kSelectionHeight = 128;
  auto selectionBase = backend.createFramebuffer(kSelectionWidth, kSelectionHeight);
  auto* selectionTarget = dynamic_cast<GraphiteFramebuffer*>(selectionBase.get());
  if (selectionTarget == nullptr) throw std::runtime_error("Graphite selection golden surface failed");
  const std::string selectionText = "abc \xD7\x90\xD7\x91\xD7\x92\nsecond line wraps here";
  const nucleus::text::TextRunView selectionRun{
      .text = {selectionText.data(), selectionText.size()}, .pointSize = 20.0f,
  };
  auto selectionParagraph = makeParagraph(&selectionRun, 1, 130.0f);
  std::uint32_t selectionRectCount = 0;
  if (!selectionParagraph.service.rectsForRange(
          selectionParagraph.value, 0, 30, nullptr, 0, &selectionRectCount
      ) || selectionRectCount < 3) {
    throw std::runtime_error("Graphite multiline selection golden did not expose per-line geometry");
  }
  std::vector<nucleus::text::TextRect> selectionRects(selectionRectCount);
  if (!selectionParagraph.service.rectsForRange(
          selectionParagraph.value, 0, 30, selectionRects.data(), selectionRects.size(), &selectionRectCount
      )) {
    throw std::runtime_error("Graphite multiline selection golden could not read selection geometry");
  }
  bool sawLtrRect = false;
  bool sawRtlRect = false;
  float firstSelectionY = selectionRects.front().y;
  bool sawAnotherLine = false;
  for (const auto& rect : selectionRects) {
    sawLtrRect |= rect.direction == nucleus::text::TextDirectionLtr;
    sawRtlRect |= rect.direction == nucleus::text::TextDirectionRtl;
    sawAnotherLine |= std::abs(rect.y - firstSelectionY) > 1.0f;
  }
  nucleus::text::TextCaret upstreamCaret{};
  nucleus::text::TextCaret downstreamCaret{};
  const bool hasDistinctBidiCarets = selectionParagraph.service.caretForOffset(
      selectionParagraph.value, 4, nucleus::text::TextAffinityUpstream, &upstreamCaret
  ) && selectionParagraph.service.caretForOffset(
      selectionParagraph.value, 4, nucleus::text::TextAffinityDownstream, &downstreamCaret
  ) && (std::abs(upstreamCaret.x - downstreamCaret.x) > 0.5f
        || std::abs(upstreamCaret.y - downstreamCaret.y) > 0.5f);
  if (!sawLtrRect || !sawRtlRect || !sawAnotherLine || !hasDistinctBidiCarets) {
    throw std::runtime_error("Graphite multiline/bidi selection geometry assertions failed");
  }
  backend.beginOffscreenFrame(*selectionTarget);
  backend.clear(rgba(0.0f, 0.0f, 0.0f, 0.0f));
  const RoundedRectStyle selectionStyle{
      .fill = rgba(0.0f, 0.75f, 1.0f, 0.45f), .softness = 0.0f, .noAa = true,
  };
  for (const auto& rect : selectionRects) {
    backend.drawRect(
        static_cast<float>(kSelectionWidth), static_cast<float>(kSelectionHeight), rect.width, rect.height,
        selectionStyle, Mat3::translation(rect.x, rect.y)
    );
  }
  backend.drawParagraph(selectionParagraph.value, 0.0f, 0.0f, Mat3::identity());
  backend.endOffscreenFrame();
  const auto selectionPixels = readPixels(graphics, *selectionTarget);
  const RegionColors selectionColors = scanRegion(
      selectionPixels, kSelectionWidth, 0, 0, kSelectionWidth, kSelectionHeight
  );
  if (!selectionColors.ink || !selectionColors.chromatic) {
    throw std::runtime_error("Graphite multiline selection rectangles did not paint");
  }

  constexpr int kImageWidth = 88;
  constexpr int kImageHeight = 40;
  std::vector<std::uint8_t> imageSource(4U * 4U * 4U, 0);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      auto* rgba8 = imageSource.data() + static_cast<std::size_t>((y * 4 + x) * 4);
      rgba8[3] = (x >= 2 && y >= 2) ? 0 : 255;
      if (x < 2 && y < 2) rgba8[0] = 255;
      if (x >= 2 && y < 2) rgba8[2] = 255;
      if (x < 2 && y >= 2) rgba8[1] = 255;
    }
  }
  TextureHandle imageTexture = backend.textureManager().loadFromPixels(
      imageSource.data(), 4, 4, TextureDataFormat::Rgba, TextureFilter::Nearest, true
  );
  if (!imageTexture.valid()) throw std::runtime_error("Graphite image golden could not upload its texture");
  auto imageBase = backend.createFramebuffer(kImageWidth, kImageHeight);
  auto* imageTarget = dynamic_cast<GraphiteFramebuffer*>(imageBase.get());
  if (imageTarget == nullptr) throw std::runtime_error("Graphite image golden could not create its surface");

  backend.beginOffscreenFrame(*imageTarget);
  backend.clear(rgba(0.0f, 0.0f, 0.0f, 0.0f));
  RenderImageDraw imageDraw{
      .texture = imageTexture.id, .surfaceWidth = kImageWidth, .surfaceHeight = kImageHeight,
      .width = 32.0f, .height = 32.0f,
  };
  backend.drawImage(imageDraw);
  imageDraw.alphaMaskTint = true;
  imageDraw.tint = rgba(1.0f, 0.0f, 1.0f, 1.0f);
  imageDraw.transform = Mat3::translation(48.0f, 0.0f);
  backend.drawImage(imageDraw);
  backend.endOffscreenFrame();
  const auto imagePixels = readPixels(graphics, *imageTarget);
  const auto* imageRed = pixel(imagePixels, kImageWidth, 8, 8);
  const auto* imageBlue = pixel(imagePixels, kImageWidth, 24, 8);
  const auto* imageGreen = pixel(imagePixels, kImageWidth, 8, 24);
  const auto* imageTransparent = pixel(imagePixels, kImageWidth, 24, 24);
  const auto* imageTint = pixel(imagePixels, kImageWidth, 56, 8);
  const auto* imageTintTransparent = pixel(imagePixels, kImageWidth, 72, 24);
  if (imageRed[0] < 240 || imageRed[1] > 10 || imageRed[2] > 10 || imageRed[3] < 240
      || imageBlue[2] < 240 || imageBlue[0] > 10 || imageBlue[3] < 240
      || imageGreen[1] < 240 || imageGreen[0] > 10 || imageGreen[3] < 240
      || imageTransparent[3] != 0
      || imageTint[0] < 240 || imageTint[1] > 10 || imageTint[2] < 240 || imageTint[3] < 240
      || imageTintTransparent[3] != 0) {
    backend.textureManager().unload(imageTexture);
    throw std::runtime_error("Graphite uploaded-image sampling/tint golden pixel assertions failed");
  }

  std::vector<std::uint8_t> replacement(4U * 4U * 4U, 0);
  for (std::size_t offset = 0; offset < replacement.size(); offset += 4) {
    replacement[offset] = 255;
    replacement[offset + 1] = 255;
    replacement[offset + 3] = 255;
  }
  if (!backend.textureManager().replace(
          imageTexture, replacement.data(), 4, 4, TextureDataFormat::Rgba, TextureFilter::Nearest, false
      )) {
    backend.textureManager().unload(imageTexture);
    throw std::runtime_error("Graphite image golden could not replace its dynamic texture");
  }
  backend.beginOffscreenFrame(*imageTarget);
  backend.clear(rgba(0.0f, 0.0f, 0.0f, 0.0f));
  imageDraw.texture = imageTexture.id;
  imageDraw.alphaMaskTint = false;
  imageDraw.tint = rgba(1.0f, 1.0f, 1.0f, 1.0f);
  imageDraw.transform = Mat3::identity();
  backend.drawImage(imageDraw);
  backend.endOffscreenFrame();
  const auto replacementPixels = readPixels(graphics, *imageTarget);
  const auto* replacementCenter = pixel(replacementPixels, kImageWidth, 16, 16);
  backend.textureManager().unload(imageTexture);
  if (replacementCenter[0] < 240 || replacementCenter[1] < 240
      || replacementCenter[2] > 10 || replacementCenter[3] < 240) {
    throw std::runtime_error("Graphite dynamic texture replacement golden pixel assertions failed");
  }

  const std::string svg =
      "data:image/svg+xml,"
      "<svg xmlns='http://www.w3.org/2000/svg' width='4' height='4' viewBox='0 0 4 4'>"
      "<rect x='0' y='0' width='2' height='2' fill='red'/>"
      "<rect x='2' y='0' width='2' height='2' fill='blue' fill-opacity='0.5'/>"
      "<rect x='0' y='2' width='2' height='2' fill='lime'/>"
      "</svg>";
  auto decodedSvg = loadImageFile(svg, 8);
  if (!decodedSvg) throw std::runtime_error("Graphite SVG golden decode failed: " + decodedSvg.error());
  TextureHandle svgTexture = backend.textureManager().loadFromRgba(
      decodedSvg->rgba.data(), decodedSvg->width, decodedSvg->height, true
  );
  if (!svgTexture.valid()) throw std::runtime_error("Graphite SVG golden could not upload its rasterized texture");
  backend.beginOffscreenFrame(*imageTarget);
  backend.clear(rgba(0.0f, 0.0f, 0.0f, 0.0f));
  imageDraw.texture = svgTexture.id;
  imageDraw.width = 32.0f;
  imageDraw.height = 32.0f;
  imageDraw.transform = Mat3::identity();
  backend.drawImage(imageDraw);
  backend.endOffscreenFrame();
  const auto svgPixels = readPixels(graphics, *imageTarget);
  const auto* svgRed = pixel(svgPixels, kImageWidth, 8, 8);
  const auto* svgBlue = pixel(svgPixels, kImageWidth, 24, 8);
  const auto* svgGreen = pixel(svgPixels, kImageWidth, 8, 24);
  const auto* svgTransparent = pixel(svgPixels, kImageWidth, 24, 24);
  backend.textureManager().unload(svgTexture);
  if (svgRed[0] < 240 || svgRed[1] > 10 || svgRed[3] < 240
      || svgBlue[2] < 115 || svgBlue[0] > 10 || !near(svgBlue[3], 128, 10)
      || svgGreen[1] < 240 || svgGreen[0] > 10 || svgGreen[3] < 240
      || svgTransparent[3] != 0) {
    throw std::runtime_error("Graphite decoded-SVG sampling golden pixel assertions failed");
  }

  constexpr int kTransitionSize = 32;
  constexpr int kTransitionCount = 6;
  auto transitionsBase = backend.createFramebuffer(kTransitionSize * kTransitionCount, kTransitionSize * 3);
  auto* transitionsTarget = dynamic_cast<GraphiteFramebuffer*>(transitionsBase.get());
  if (transitionsTarget == nullptr) {
    throw std::runtime_error("Graphite wallpaper-transition golden could not create its surface");
  }
  backend.beginOffscreenFrame(*transitionsTarget);
  backend.clear(rgba(0.0f, 0.0f, 0.0f, 0.0f));
  WallpaperDrawParams transitionDraw{
      .from = {.kind = WallpaperSourceKind::Color, .color = rgba(1.0f, 0.0f, 0.0f, 1.0f)},
      .to = {.kind = WallpaperSourceKind::Color, .color = rgba(0.0f, 1.0f, 0.0f, 1.0f)},
      .surfaceWidth = kTransitionSize * kTransitionCount,
      .surfaceHeight = kTransitionSize * 3,
      .quadWidth = kTransitionSize,
      .quadHeight = kTransitionSize,
  };
  transitionDraw.params.aspectRatio = 1.0f;
  transitionDraw.params.smoothness = 0.25f;
  for (int index = 0; index < kTransitionCount; ++index) {
    transitionDraw.transition = static_cast<WallpaperTransition>(index);
    transitionDraw.progress = 0.0f;
    transitionDraw.transform = Mat3::translation(static_cast<float>(index * kTransitionSize), 0.0f);
    backend.drawWallpaper(transitionDraw);
    transitionDraw.progress = 1.0f;
    transitionDraw.transform = Mat3::translation(
        static_cast<float>(index * kTransitionSize), static_cast<float>(kTransitionSize)
    );
    backend.drawWallpaper(transitionDraw);
    transitionDraw.progress = 0.5f;
    transitionDraw.transform = Mat3::translation(
        static_cast<float>(index * kTransitionSize), static_cast<float>(kTransitionSize * 2)
    );
    backend.drawWallpaper(transitionDraw);
  }
  backend.endOffscreenFrame();
  const auto transitionPixels = readPixels(graphics, *transitionsTarget);
  GraphiteRuntimeEffects referenceEffects;
  for (int index = 0; index < kTransitionCount; ++index) {
    const int x = index * kTransitionSize + kTransitionSize / 2;
    const auto* start = pixel(transitionPixels, kTransitionSize * kTransitionCount, x, kTransitionSize / 2);
    const auto* end = pixel(
        transitionPixels, kTransitionSize * kTransitionCount, x, kTransitionSize + kTransitionSize / 2
    );
    if (start[0] < 235 || start[1] > 20 || start[3] < 240
        || end[1] < 235 || end[0] > 20 || end[3] < 240) {
      throw std::runtime_error(
          "Graphite wallpaper transition endpoint golden failed for effect " + std::to_string(index)
      );
    }
    const auto effectId = static_cast<GraphiteRuntimeEffectId>(
        static_cast<std::size_t>(GraphiteRuntimeEffectId::WallpaperFade) + static_cast<std::size_t>(index)
    );
    const auto cpuReference = renderWallpaperCpuReference(referenceEffects.get(effectId), kTransitionSize, 0.5f);
    requireCpuGpuReferenceMatch(
        transitionPixels, kTransitionSize * kTransitionCount, index * kTransitionSize, kTransitionSize * 2,
        cpuReference, kTransitionSize, index
    );
  }

  constexpr int kEffectTile = 48;
  constexpr int kWeatherCount = 6;
  auto effectsBase = backend.createFramebuffer(kEffectTile * kWeatherCount, kEffectTile * 2);
  auto* effectsTarget = dynamic_cast<GraphiteFramebuffer*>(effectsBase.get());
  if (effectsTarget == nullptr) throw std::runtime_error("Graphite procedural-effect golden surface failed");
  backend.beginOffscreenFrame(*effectsTarget);
  backend.clear(rgba(0.0f, 0.0f, 0.0f, 0.0f));

  RoundedRectStyle advancedRect;
  advancedRect.fill = rgba(0.0f, 1.0f, 1.0f, 1.0f);
  advancedRect.radius = Radii(14.0f);
  advancedRect.corners.tl = CornerShape::Concave;
  advancedRect.border = rgba(1.0f, 0.0f, 1.0f, 1.0f);
  advancedRect.borderWidth = 2.0f;
  backend.drawRect(
      kEffectTile * kWeatherCount, kEffectTile * 2, kEffectTile, kEffectTile, advancedRect, Mat3::identity()
  );
  const ScreenCornerStyle cornerStyle{
      .color = rgba(1.0f, 1.0f, 0.0f, 1.0f), .position = ScreenCornerPosition::TopLeft,
      .exponent = 4.0f, .softness = 1.0f,
  };
  backend.drawScreenCorner(
      kEffectTile * kWeatherCount, kEffectTile * 2, 1.0f, 1.0f, kEffectTile, kEffectTile, cornerStyle,
      Mat3::translation(static_cast<float>(kEffectTile), 0.0f)
  );

  EffectStyle weather{
      .time = 7.25f,
      .radius = 6.0f,
      .bgColor = rgba(0.0f, 0.0f, 0.0f, 1.0f),
      .skyTop = rgba(0.25f, 0.65f, 1.0f, 1.0f),
      .skyBottom = rgba(0.02f, 0.12f, 0.35f, 1.0f),
      .cloudAmount = 0.5f,
      .intensity = 1.0f,
  };
  for (int index = 0; index < kWeatherCount; ++index) {
    weather.type = static_cast<EffectType>(index + 1);
    backend.drawEffect(
        kEffectTile * kWeatherCount, kEffectTile * 2, kEffectTile, kEffectTile, weather,
        Mat3::translation(static_cast<float>(index * kEffectTile), static_cast<float>(kEffectTile))
    );
  }
  backend.endOffscreenFrame();
  const auto effectPixels = readPixels(graphics, *effectsTarget);
  const int effectWidth = kEffectTile * kWeatherCount;
  const auto* advancedCenter = pixel(effectPixels, effectWidth, kEffectTile / 2, kEffectTile / 2);
  const auto* cornerFilled = pixel(effectPixels, effectWidth, kEffectTile + 1, 1);
  const auto* cornerClear = pixel(effectPixels, effectWidth, kEffectTile * 2 - 2, kEffectTile - 2);
  if (advancedCenter[1] < 220 || advancedCenter[2] < 220 || advancedCenter[3] < 240
      || cornerFilled[0] < 220 || cornerFilled[1] < 220 || cornerFilled[3] < 220
      || cornerClear[3] > 20) {
    throw std::runtime_error("Graphite advanced-rectangle/screen-corner GPU golden failed");
  }
  for (int index = 0; index < kWeatherCount; ++index) {
    const int x = index * kEffectTile + kEffectTile / 2;
    const auto* upper = pixel(effectPixels, effectWidth, x, kEffectTile + 8);
    const auto* lower = pixel(effectPixels, effectWidth, x, kEffectTile * 2 - 9);
    const int colorDifference = std::abs(static_cast<int>(upper[0]) - static_cast<int>(lower[0]))
        + std::abs(static_cast<int>(upper[1]) - static_cast<int>(lower[1]))
        + std::abs(static_cast<int>(upper[2]) - static_cast<int>(lower[2]));
    if (upper[3] < 220 || lower[3] < 220 || colorDifference < 8) {
      throw std::runtime_error("Graphite weather-effect GPU golden failed for effect " + std::to_string(index));
    }
    constexpr std::array<GraphiteRuntimeEffectId, kWeatherCount> weatherEffects{
        GraphiteRuntimeEffectId::WeatherSky,
        GraphiteRuntimeEffectId::WeatherCloud,
        GraphiteRuntimeEffectId::WeatherCloud,
        GraphiteRuntimeEffectId::WeatherRain,
        GraphiteRuntimeEffectId::WeatherSnow,
        GraphiteRuntimeEffectId::WeatherThunder,
    };
    const auto cpuReference = renderWeatherCpuReference(
        referenceEffects.get(weatherEffects[static_cast<std::size_t>(index)]), kEffectTile, index == 2
    );
    requireCpuGpuReferenceMatch(
        effectPixels, effectWidth, index * kEffectTile, kEffectTile, cpuReference, kEffectTile,
        kTransitionCount + index, 8.0, 0.16
    );
  }

  constexpr int kDataWidth = 32;
  std::vector<std::uint8_t> dynamicData(static_cast<std::size_t>(kDataWidth) * 4U, 255);
  for (int index = 0; index < kDataWidth; ++index) {
    const float phase = static_cast<float>(index) / static_cast<float>(kDataWidth - 1);
    dynamicData[static_cast<std::size_t>(index) * 4U] = static_cast<std::uint8_t>(phase * 255.0f);
    dynamicData[static_cast<std::size_t>(index) * 4U + 1U] =
        static_cast<std::uint8_t>((1.0f - phase) * 255.0f);
    dynamicData[static_cast<std::size_t>(index) * 4U + 2U] =
        static_cast<std::uint8_t>((0.5f + 0.4f * std::sin(phase * 12.56637f)) * 255.0f);
  }
  TextureHandle dynamicTexture = backend.textureManager().loadFromPixels(
      dynamicData.data(), kDataWidth, 1, TextureDataFormat::Rgba, TextureFilter::Nearest, false
  );
  if (!dynamicTexture.valid()) throw std::runtime_error("Graphite data-driven effect texture upload failed");
  constexpr int kDynamicWidth = 192;
  constexpr int kDynamicHeight = 96;
  auto dynamicBase = backend.createFramebuffer(kDynamicWidth, kDynamicHeight);
  auto* dynamicTarget = dynamic_cast<GraphiteFramebuffer*>(dynamicBase.get());
  if (dynamicTarget == nullptr) throw std::runtime_error("Graphite data-driven effect surface failed");
  backend.beginOffscreenFrame(*dynamicTarget);
  backend.clear(rgba(0.0f, 0.0f, 0.0f, 0.0f));
  const GraphStyle graphStyle{
      .lineColor1 = rgba(1.0f, 0.0f, 0.0f, 1.0f), .count1 = 28.0f,
      .lineColor2 = rgba(0.0f, 1.0f, 0.0f, 1.0f), .count2 = 28.0f,
      .lineColor3 = rgba(0.0f, 0.0f, 1.0f, 1.0f), .count3 = 28.0f,
      .lineWidth = 2.0f, .graphFillOpacity = 0.2f,
  };
  backend.drawGraph(
      dynamicTexture.id, kDataWidth, kDynamicWidth, kDynamicHeight, 96.0f, 96.0f, graphStyle, Mat3::identity()
  );
  const FancyAudioVisualizerStyle visualizerStyle{
      .primaryColor = rgba(1.0f, 0.1f, 0.2f, 1.0f),
      .secondaryColor = rgba(0.1f, 0.5f, 1.0f, 1.0f),
      .mode = FancyAudioVisualizerMode::All,
      .time = 3.5f,
      .sensitivity = 1.0f,
  };
  backend.drawFancyAudioVisualizer(
      dynamicTexture.id, kDataWidth, kDynamicWidth, kDynamicHeight, 96.0f, 96.0f, visualizerStyle,
      Mat3::translation(96.0f, 0.0f)
  );
  backend.endOffscreenFrame();
  const auto dynamicPixels = readPixels(graphics, *dynamicTarget);
  backend.textureManager().unload(dynamicTexture);
  const RegionColors graphColors = scanRegion(dynamicPixels, kDynamicWidth, 0, 0, 96, 96);
  const RegionColors visualizerColors = scanRegion(dynamicPixels, kDynamicWidth, 96, 0, 192, 96);
  if (!graphColors.ink || !graphColors.red || !graphColors.green || !graphColors.blue
      || !visualizerColors.ink || !visualizerColors.chromatic) {
    throw std::runtime_error("Graphite graph/fancy-visualizer GPU golden failed");
  }
  const auto graphReference = renderDataDrivenCpuReference(
      referenceEffects.get(GraphiteRuntimeEffectId::Graph), dynamicData, kDataWidth, kDynamicHeight, false
  );
  requireCpuGpuReferenceMatch(
      dynamicPixels, kDynamicWidth, 0, 0, graphReference, kDynamicHeight, 12, 8.0, 0.16
  );
  const auto visualizerReference = renderDataDrivenCpuReference(
      referenceEffects.get(GraphiteRuntimeEffectId::FancyAudioVisualizer), dynamicData, kDataWidth, kDynamicHeight,
      true
  );
  requireCpuGpuReferenceMatch(
      dynamicPixels, kDynamicWidth, 96, 0, visualizerReference, kDynamicHeight, 13, 8.0, 0.16
  );

  constexpr int kNativeTile = 48;
  constexpr int kNativeWidth = kNativeTile * 4;
  auto nativeBase = backend.createFramebuffer(kNativeWidth, kNativeTile);
  auto* nativeTarget = dynamic_cast<GraphiteFramebuffer*>(nativeBase.get());
  if (nativeTarget == nullptr) throw std::runtime_error("Graphite native-primitive golden surface failed");
  backend.beginOffscreenFrame(*nativeTarget);
  backend.clear(rgba(0.0f, 0.0f, 0.0f, 0.0f));
  backend.drawSpinner(
      kNativeWidth, kNativeTile, 40.0f, 40.0f,
      SpinnerStyle{.color = rgba(1.0f, 0.0f, 0.0f, 1.0f), .thickness = 4.0f},
      Mat3::translation(4.0f, 4.0f)
  );
  backend.drawCountdownRing(
      kNativeWidth, kNativeTile, 40.0f, 40.0f,
      CountdownRingStyle{.color = rgba(0.0f, 1.0f, 0.0f, 1.0f), .thickness = 4.0f, .progress = 0.5f},
      Mat3::translation(52.0f, 4.0f)
  );
  constexpr std::array<float, 5> spectrumValues{0.2f, 0.45f, 0.7f, 1.0f, 0.55f};
  backend.drawAudioSpectrum(
      kNativeWidth, kNativeTile, 1.0f, 1.0f, 40.0f, 40.0f,
      AudioSpectrumStyle{
          .color1 = rgba(0.0f, 0.2f, 1.0f, 1.0f), .color2 = rgba(1.0f, 0.0f, 1.0f, 1.0f),
          .orientation = AudioSpectrumOrientation::Horizontal,
      },
      spectrumValues, Mat3::translation(100.0f, 4.0f)
  );
  RoundedRectStyle gradient;
  gradient.fillMode = FillMode::LinearGradient;
  gradient.gradientDirection = GradientDirection::Horizontal;
  gradient.gradientStops = {
      GradientStop{0.0f, rgba(1.0f, 0.0f, 0.0f, 1.0f)}, GradientStop{0.33f, rgba(1.0f, 0.0f, 0.0f, 1.0f)},
      GradientStop{0.67f, rgba(0.0f, 1.0f, 0.0f, 1.0f)}, GradientStop{1.0f, rgba(0.0f, 1.0f, 0.0f, 1.0f)},
  };
  backend.setScissor(RenderScissor{.x = 152, .y = 8, .width = 32, .height = 24});
  backend.drawRect(kNativeWidth, kNativeTile, 40.0f, 40.0f, gradient, Mat3::translation(148.0f, 4.0f));
  backend.disableScissor();
  backend.endOffscreenFrame();
  const auto nativePixels = readPixels(graphics, *nativeTarget);
  const RegionColors spinnerColors = scanRegion(nativePixels, kNativeWidth, 0, 0, 48, 48);
  const RegionColors countdownColors = scanRegion(nativePixels, kNativeWidth, 48, 0, 96, 48);
  const RegionColors spectrumColors = scanRegion(nativePixels, kNativeWidth, 96, 0, 144, 48);
  const auto* clippedOutside = pixel(nativePixels, kNativeWidth, 150, 20);
  const auto* gradientLeft = pixel(nativePixels, kNativeWidth, 156, 20);
  const auto* gradientRight = pixel(nativePixels, kNativeWidth, 180, 20);
  if (!spinnerColors.red || !countdownColors.green
      || !spectrumColors.blue || !spectrumColors.chromatic
      || clippedOutside[3] != 0
      || gradientLeft[0] < 180 || gradientLeft[1] > 100 || gradientLeft[3] < 240
      || gradientRight[1] < 180 || gradientRight[0] > 100 || gradientRight[3] < 240) {
    std::ostringstream message;
    message << "Graphite native primitive/transform/scissor golden failed: spinner=" << spinnerColors.red
            << " countdown=" << countdownColors.green
            << " spectrum=" << spectrumColors.red << ',' << spectrumColors.blue << ',' << spectrumColors.chromatic
            << " outsideA=" << +clippedOutside[3]
            << " left=" << +gradientLeft[0] << ',' << +gradientLeft[1] << ',' << +gradientLeft[2] << ','
            << +gradientLeft[3] << " right=" << +gradientRight[0] << ',' << +gradientRight[1] << ','
            << +gradientRight[2] << ',' << +gradientRight[3];
    throw std::runtime_error(message.str());
  }

  constexpr int kBlendWidth = 48;
  constexpr int kBlendHeight = 16;
  auto blendBase = backend.createFramebuffer(kBlendWidth, kBlendHeight);
  auto* blendTarget = dynamic_cast<GraphiteFramebuffer*>(blendBase.get());
  if (blendTarget == nullptr) throw std::runtime_error("Graphite blend golden surface failed");
  backend.beginOffscreenFrame(*blendTarget);
  backend.clear(rgba(0.0f, 0.0f, 1.0f, 1.0f));
  RoundedRectStyle translucentRed;
  translucentRed.fill = rgba(1.0f, 0.0f, 0.0f, 0.5f);
  backend.setBlendMode(RenderBlendMode::PremultipliedAlpha);
  backend.drawRect(kBlendWidth, kBlendHeight, 16.0f, 16.0f, translucentRed, Mat3::identity());
  backend.setBlendMode(RenderBlendMode::StraightAlpha);
  backend.drawRect(
      kBlendWidth, kBlendHeight, 16.0f, 16.0f, translucentRed, Mat3::translation(16.0f, 0.0f)
  );
  RoundedRectStyle translucentGreen;
  translucentGreen.fill = rgba(0.0f, 1.0f, 0.0f, 0.5f);
  backend.setBlendMode(RenderBlendMode::Disabled);
  backend.drawRect(
      kBlendWidth, kBlendHeight, 16.0f, 16.0f, translucentGreen, Mat3::translation(32.0f, 0.0f)
  );
  backend.setBlendMode(RenderBlendMode::PremultipliedAlpha);
  backend.endOffscreenFrame();
  const auto blendPixels = readPixels(graphics, *blendTarget);
  const auto* premultipliedBlend = pixel(blendPixels, kBlendWidth, 8, 8);
  const auto* straightBlend = pixel(blendPixels, kBlendWidth, 24, 8);
  const auto* replacementBlend = pixel(blendPixels, kBlendWidth, 40, 8);
  if (!near(premultipliedBlend[0], 128, 4) || premultipliedBlend[1] > 4
      || !near(premultipliedBlend[2], 127, 4) || premultipliedBlend[3] < 250
      || !near(straightBlend[0], 128, 4) || straightBlend[1] > 4
      || !near(straightBlend[2], 127, 4) || straightBlend[3] < 250
      || replacementBlend[0] > 4 || !near(replacementBlend[1], 128, 4)
      || replacementBlend[2] > 4 || !near(replacementBlend[3], 128, 4)) {
    throw std::runtime_error("Graphite explicit blend-mode golden failed");
  }

  constexpr int kCheckerSize = 64;
  std::vector<std::uint8_t> checker(static_cast<std::size_t>(kCheckerSize * kCheckerSize * 4));
  for (int y = 0; y < kCheckerSize; ++y) {
    for (int x = 0; x < kCheckerSize; ++x) {
      const std::uint8_t value = (x + y) % 2 == 0 ? 0 : 255;
      auto* rgba8 = checker.data() + static_cast<std::size_t>((y * kCheckerSize + x) * 4);
      rgba8[0] = value;
      rgba8[1] = value;
      rgba8[2] = value;
      rgba8[3] = 255;
    }
  }
  TextureHandle checkerTexture = backend.textureManager().loadFromPixels(
      checker.data(), kCheckerSize, kCheckerSize, TextureDataFormat::Rgba, TextureFilter::Linear, true
  );
  if (!checkerTexture.valid()) throw std::runtime_error("Graphite mipmap golden texture upload failed");
  const SkImage* checkerImage = graphics.textureManager().image(checkerTexture.id);
  if (checkerImage == nullptr || !checkerImage->hasMipmaps()) {
    backend.textureManager().unload(checkerTexture);
    throw std::runtime_error("Graphite mipmap golden image does not expose its uploaded mip chain");
  }
  auto minifyBase = backend.createFramebuffer(16, 16);
  auto* minifyTarget = dynamic_cast<GraphiteFramebuffer*>(minifyBase.get());
  if (minifyTarget == nullptr) throw std::runtime_error("Graphite mipmap golden surface failed");
  backend.beginOffscreenFrame(*minifyTarget);
  backend.clear(rgba(0.0f, 0.0f, 0.0f, 0.0f));
  backend.drawImage(RenderImageDraw{
      .texture = checkerTexture.id,
      .surfaceWidth = 16.0f,
      .surfaceHeight = 16.0f,
      .width = 8.0f,
      .height = 8.0f,
      .textureWidth = static_cast<float>(kCheckerSize),
      .textureHeight = static_cast<float>(kCheckerSize),
      .transform = Mat3::translation(4.0f, 4.0f),
  });
  const std::array<std::uint8_t, 4> encodedMidGray{188, 188, 188, 255};
  TextureHandle midGrayTexture = backend.textureManager().loadFromPixels(
      encodedMidGray.data(), 1, 1, TextureDataFormat::Rgba, TextureFilter::Nearest, false
  );
  if (!midGrayTexture.valid()) throw std::runtime_error("Graphite color-space control texture upload failed");
  backend.drawImage(RenderImageDraw{
      .texture = midGrayTexture.id,
      .surfaceWidth = 16.0f,
      .surfaceHeight = 16.0f,
      .width = 4.0f,
      .height = 4.0f,
      .textureWidth = 1.0f,
      .textureHeight = 1.0f,
      .transform = Mat3::translation(0.0f, 0.0f),
  });
  backend.endOffscreenFrame();
  const auto minifyPixels = readPixels(graphics, *minifyTarget);
  backend.textureManager().unload(checkerTexture);
  backend.textureManager().unload(midGrayTexture);
  const int controlGray = static_cast<int>(pixel(minifyPixels, 16, 1, 1)[0]);
  if (controlGray < 185 || controlGray > 191) {
    throw std::runtime_error("Graphite sRGB destination-native color control failed");
  }
  int minimumGray = 255;
  int maximumGray = 0;
  std::uint64_t grayTotal = 0;
  for (int y = 4; y < 12; ++y) {
    for (int x = 4; x < 12; ++x) {
      const auto* rgba8 = pixel(minifyPixels, 16, x, y);
      minimumGray = std::min(minimumGray, static_cast<int>(rgba8[0]));
      maximumGray = std::max(maximumGray, static_cast<int>(rgba8[0]));
      grayTotal += rgba8[0];
      if (std::abs(static_cast<int>(rgba8[0]) - static_cast<int>(rgba8[1])) > 2
          || std::abs(static_cast<int>(rgba8[0]) - static_cast<int>(rgba8[2])) > 2
          || rgba8[3] < 250) {
        throw std::runtime_error("Graphite mipmap/color-space golden produced non-gray or translucent output");
      }
    }
  }
  const double meanGray = static_cast<double>(grayTotal) / 64.0;
  if (meanGray < 165.0 || meanGray > 205.0 || maximumGray - minimumGray > 12) {
    std::ostringstream message;
    message << "Graphite sRGB mipmap/downsampling golden failed: mean=" << meanGray
            << " range=" << minimumGray << '-' << maximumGray << " control=" << controlGray;
    throw std::runtime_error(message.str());
  }
}
