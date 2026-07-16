#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkShader.h"
#include "include/core/SkString.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypes.h"
#include "include/effects/SkRuntimeEffect.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
  void setFloat(SkRuntimeShaderBuilder& builder, std::string_view name, float value) {
    if (builder.effect()->findUniform(name) != nullptr) builder.uniform(name) = value;
  }

  void setFloat2(SkRuntimeShaderBuilder& builder, std::string_view name, SkV2 value) {
    if (builder.effect()->findUniform(name) != nullptr) builder.uniform(name) = value;
  }

  void setFloat4(SkRuntimeShaderBuilder& builder, std::string_view name, SkV4 value) {
    if (builder.effect()->findUniform(name) != nullptr) builder.uniform(name) = value;
  }

  void setInt(SkRuntimeShaderBuilder& builder, std::string_view name, std::int32_t value) {
    if (builder.effect()->findUniform(name) != nullptr) builder.uniform(name) = value;
  }

  struct PixelStats {
    std::uint64_t opaque = 0;
    std::uint64_t transparent = 0;
    std::uint64_t colored = 0;
    SkColor center = SK_ColorTRANSPARENT;
    SkColor corner = SK_ColorTRANSPARENT;
  };

  PixelStats renderStats(SkRuntimeShaderBuilder& builder, int width = 96, int height = 64) {
    PixelStats stats;
    auto shader = builder.makeShader();
    auto surface = SkSurfaces::Raster(
        SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType)
    );
    if (shader == nullptr || surface == nullptr) return stats;
    SkPaint paint;
    paint.setShader(std::move(shader));
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    surface->getCanvas()->drawPaint(paint);
    SkPixmap pixels;
    if (!surface->peekPixels(&pixels)) return stats;
    stats.center = pixels.getColor(width / 2, height / 2);
    stats.corner = pixels.getColor(0, 0);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const SkColor color = pixels.getColor(x, y);
        const auto alpha = SkColorGetA(color);
        stats.opaque += alpha > 240;
        stats.transparent += alpha < 4;
        stats.colored += alpha > 4 && (SkColorGetR(color) > 4 || SkColorGetG(color) > 4 || SkColorGetB(color) > 4);
      }
    }
    return stats;
  }

  bool proceduralEffectGolden(std::string_view name, const sk_sp<SkRuntimeEffect>& effect) {
    constexpr float width = 96.0f;
    constexpr float height = 64.0f;
    SkRuntimeShaderBuilder builder(effect);
    for (const auto& child : effect->children()) {
      builder.child(child.name) = SkShaders::Color(SkColorSetARGB(255, 180, 128, 220));
    }

    if (name == "advanced_rect.sksl") {
      setFloat2(builder, "u_rect_size", SkV2{80.0f, 48.0f});
      setFloat2(builder, "u_rect_origin", SkV2{8.0f, 8.0f});
      setFloat4(builder, "u_color", SkV4{1.0f, 0.0f, 0.0f, 1.0f});
      setFloat4(builder, "u_border_color", SkV4{0.0f, 0.0f, 1.0f, 1.0f});
      setFloat(builder, "u_fill_mode", 1.0f);
      setFloat2(builder, "u_gradient_direction", SkV2{1.0f, 0.0f});
      setFloat4(builder, "u_gradient_stops", SkV4{0.0f, 0.33f, 0.66f, 1.0f});
      setFloat4(builder, "u_gradient_color0", SkV4{1.0f, 0.0f, 0.0f, 1.0f});
      setFloat4(builder, "u_gradient_color1", SkV4{1.0f, 0.0f, 0.0f, 1.0f});
      setFloat4(builder, "u_gradient_color2", SkV4{1.0f, 0.0f, 0.0f, 1.0f});
      setFloat4(builder, "u_gradient_color3", SkV4{1.0f, 0.0f, 0.0f, 1.0f});
      setFloat4(builder, "u_corner_shapes", SkV4{0.0f, 0.0f, 0.0f, 0.0f});
      setFloat4(builder, "u_logical_inset", SkV4{0.0f, 0.0f, 0.0f, 0.0f});
      setFloat4(builder, "u_radii", SkV4{12.0f, 12.0f, 12.0f, 12.0f});
      setFloat(builder, "u_softness", 1.0f);
      setFloat(builder, "u_border_width", 3.0f);
      const auto stats = renderStats(builder);
      return SkColorGetA(stats.center) > 240 && SkColorGetR(stats.center) > 220
          && stats.opaque > 1000 && stats.transparent > 500 && stats.colored > 1000;
    }

    if (name == "screen_corner.sksl") {
      setFloat2(builder, "size", SkV2{width, height});
      setFloat2(builder, "pixelScale", SkV2{1.5f, 1.5f});
      setFloat4(builder, "color", SkV4{0.0f, 1.0f, 0.0f, 1.0f});
      setInt(builder, "corner", 0);
      setFloat(builder, "exponent", 2.0f);
      setFloat(builder, "softness", 1.0f);
      const auto stats = renderStats(builder);
      return stats.opaque > 500 && stats.transparent > 500 && stats.colored > 500;
    }

    if (name.starts_with("effect_")) {
      setFloat(builder, "u_time", 7.0f);
      setFloat(builder, "u_item_width", width);
      setFloat(builder, "u_item_height", height);
      setFloat4(builder, "u_bg_color", SkV4{0.1f, 0.2f, 0.3f, 1.0f});
      setFloat(builder, "u_radius", 10.0f);
      setFloat(builder, "u_alternative", 0.0f);
      setFloat(builder, "u_night", 0.0f);
      setFloat(builder, "u_cloud_amount", 0.5f);
      setFloat(builder, "u_intensity", 0.8f);
      if (builder.effect()->findUniform("u_sky_top") != nullptr)
        builder.uniform("u_sky_top") = SkV3{0.2f, 0.5f, 0.9f};
      if (builder.effect()->findUniform("u_sky_bottom") != nullptr)
        builder.uniform("u_sky_bottom") = SkV3{0.7f, 0.8f, 1.0f};
      const auto stats = renderStats(builder);
      return SkColorGetA(stats.center) > 240 && SkColorGetA(stats.corner) < 8
          && stats.opaque > 3000 && stats.transparent > 20 && stats.colored > 3000;
    }

    if (name == "graph.sksl") {
      setFloat4(builder, "u_line_color1", SkV4{1.0f, 0.0f, 0.0f, 1.0f});
      setFloat4(builder, "u_line_color2", SkV4{0.0f, 1.0f, 0.0f, 1.0f});
      setFloat4(builder, "u_line_color3", SkV4{0.0f, 0.0f, 1.0f, 1.0f});
      setFloat(builder, "u_count1", 64.0f);
      setFloat(builder, "u_count2", 64.0f);
      setFloat(builder, "u_count3", 64.0f);
      setFloat(builder, "u_line_width", 2.0f);
      setFloat(builder, "u_graph_fill_opacity", 0.25f);
      setFloat(builder, "u_tex_width", 64.0f);
      setFloat(builder, "u_res_x", width);
      setFloat(builder, "u_res_y", height);
      setFloat(builder, "u_aa_size", 1.0f);
      const auto stats = renderStats(builder);
      return stats.colored > 100 && stats.transparent > 100;
    }

    if (name == "fancy_audio_visualizer.sksl") {
      setFloat(builder, "u_texture_width", 64.0f);
      setFloat(builder, "u_time", 12.0f);
      setFloat(builder, "u_item_width", width);
      setFloat(builder, "u_item_height", height);
      setFloat4(builder, "u_primary_color", SkV4{0.9f, 0.2f, 0.8f, 1.0f});
      setFloat4(builder, "u_secondary_color", SkV4{0.1f, 0.8f, 1.0f, 1.0f});
      setFloat(builder, "u_sensitivity", 1.0f);
      setFloat(builder, "u_rotation_speed", 1.0f);
      setFloat(builder, "u_bar_width", 0.5f);
      setFloat(builder, "u_ring_opacity", 0.8f);
      setFloat(builder, "u_corner_radius", 8.0f);
      setFloat(builder, "u_bloom_intensity", 1.0f);
      setFloat(builder, "u_mode", 5.0f);
      setFloat(builder, "u_wave_thickness", 0.02f);
      setFloat(builder, "u_inner_diameter", 0.3f);
      const auto stats = renderStats(builder);
      return stats.colored > 100 && stats.transparent > 100;
    }
    return true;
  }

  bool wallpaperPixelGolden(const sk_sp<SkRuntimeEffect>& effect, float progress, bool expectBlue) {
    constexpr int size = 64;
    SkRuntimeShaderBuilder builder(effect);
    for (const auto& child : effect->children()) {
      builder.child(child.name) = SkShaders::Color(SK_ColorTRANSPARENT);
    }
    setFloat(builder, "u_sourceKind1", 1.0f);
    setFloat(builder, "u_sourceKind2", 1.0f);
    setFloat4(builder, "u_sourceColor1", SkV4{1.0f, 0.0f, 0.0f, 1.0f});
    setFloat4(builder, "u_sourceColor2", SkV4{0.0f, 0.0f, 1.0f, 1.0f});
    setFloat(builder, "u_progress", progress);
    setFloat(builder, "u_fillMode", 3.0f);
    setFloat(builder, "u_imageWidth1", static_cast<float>(size));
    setFloat(builder, "u_imageHeight1", static_cast<float>(size));
    setFloat(builder, "u_imageWidth2", static_cast<float>(size));
    setFloat(builder, "u_imageHeight2", static_cast<float>(size));
    setFloat(builder, "u_screenWidth", static_cast<float>(size));
    setFloat(builder, "u_screenHeight", static_cast<float>(size));
    setFloat4(builder, "u_fillColor", SkV4{0.0f, 0.0f, 0.0f, 1.0f});
    setFloat2(builder, "u_spanOffset", SkV2{0.0f, 0.0f});
    setFloat2(builder, "u_spanMonitorSize", SkV2{static_cast<float>(size), static_cast<float>(size)});
    setFloat2(builder, "u_spanTotalSize", SkV2{static_cast<float>(size), static_cast<float>(size)});
    setFloat(builder, "u_direction", 0.0f);
    setFloat(builder, "u_smoothness", 0.1f);
    setFloat(builder, "u_centerX", 0.5f);
    setFloat(builder, "u_centerY", 0.5f);
    setFloat(builder, "u_aspectRatio", 1.0f);
    setFloat(builder, "u_stripeCount", 8.0f);
    setFloat(builder, "u_angle", 25.0f);
    setFloat(builder, "u_cellSize", 0.12f);

    auto shader = builder.makeShader();
    auto surface = SkSurfaces::Raster(
        SkImageInfo::Make(size, size, kRGBA_8888_SkColorType, kPremul_SkAlphaType)
    );
    if (shader == nullptr || surface == nullptr) return false;
    SkPaint paint;
    paint.setShader(std::move(shader));
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    surface->getCanvas()->drawPaint(paint);
    SkPixmap pixels;
    if (!surface->peekPixels(&pixels)) return false;
    std::uint64_t red = 0, blue = 0, alpha = 0;
    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        const SkColor color = pixels.getColor(x, y);
        red += SkColorGetR(color);
        blue += SkColorGetB(color);
        alpha += SkColorGetA(color);
      }
    }
    const std::uint64_t count = static_cast<std::uint64_t>(size * size);
    if (progress > 0.0f && progress < 1.0f) {
      return alpha / count == 255 && red / count > 30 && blue / count > 30;
    }
    const std::uint64_t expected = expectBlue ? blue : red;
    const std::uint64_t rejected = expectBlue ? red : blue;
    return alpha / count == 255 && expected > rejected * 5 && expected / count > 220;
  }
} // namespace

int main() {
  constexpr std::array<std::string_view, 15> expectedShaders = {
      "advanced_rect.sksl",
      "effect_cloud.sksl",
      "effect_rain.sksl",
      "effect_sky.sksl",
      "effect_snow.sksl",
      "effect_thunder.sksl",
      "fancy_audio_visualizer.sksl",
      "graph.sksl",
      "screen_corner.sksl",
      "wallpaper_disc.sksl",
      "wallpaper_fade.sksl",
      "wallpaper_honeycomb.sksl",
      "wallpaper_stripes.sksl",
      "wallpaper_wipe.sksl",
      "wallpaper_zoom.sksl",
  };
  const std::filesystem::path directory = std::filesystem::path(NOCTALIA_SOURCE_ASSETS_DIR) / "shaders";
  std::vector<std::filesystem::path> shaders;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_regular_file() && entry.path().extension() == ".sksl") {
      shaders.push_back(entry.path());
    }
  }
  std::ranges::sort(shaders);
  std::vector<std::string> shaderNames;
  shaderNames.reserve(shaders.size());
  for (const auto& shader : shaders) {
    shaderNames.push_back(shader.filename().string());
  }
  if (!std::ranges::equal(shaderNames, expectedShaders)) {
    std::cerr << "runtime-effect asset inventory does not match the registered family\n";
    return 1;
  }

  bool failed = false;
  for (const auto& path : shaders) {
    std::ifstream stream(path, std::ios::binary);
    const std::string source(std::istreambuf_iterator<char>(stream), {});
    if (!stream.good() && !stream.eof()) {
      std::cerr << "failed to read " << path << '\n';
      failed = true;
      continue;
    }
    auto result = SkRuntimeEffect::MakeForShader(SkString(source));
    if (result.effect == nullptr) {
      std::cerr << path << ": " << result.errorText.c_str() << '\n';
      failed = true;
      continue;
    }

    SkRuntimeShaderBuilder builder(result.effect);
    for (const auto& child : result.effect->children()) {
      if (child.type != SkRuntimeEffect::ChildType::kShader) {
        std::cerr << path << ": runtime-effect child is not a shader\n";
        failed = true;
        continue;
      }
      builder.child(child.name) = SkShaders::Color(SK_ColorTRANSPARENT);
    }
    if (builder.makeShader() == nullptr) {
      std::cerr << path << ": failed to instantiate runtime shader\n";
      failed = true;
    }
    if (path.filename().string().starts_with("wallpaper_")) {
      if (!wallpaperPixelGolden(result.effect, 0.0f, false)) {
        std::cerr << path << ": progress=0 endpoint pixel golden drifted\n";
        failed = true;
      }
      if (!wallpaperPixelGolden(result.effect, 0.5f, false)) {
        std::cerr << path << ": progress=0.5 transition pixel golden drifted\n";
        failed = true;
      }
      if (!wallpaperPixelGolden(result.effect, 1.0f, true)) {
        std::cerr << path << ": progress=1 endpoint pixel golden drifted\n";
        failed = true;
      }
    } else if (!proceduralEffectGolden(path.filename().string(), result.effect)) {
      std::cerr << path << ": procedural pixel golden drifted\n";
      failed = true;
    }
  }
  return failed ? 1 : 0;
}
