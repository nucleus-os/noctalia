#include "render/backend/graphite_runtime_effect.h"

#include "core/files/resource_paths.h"
#include "include/core/SkString.h"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

sk_sp<SkRuntimeEffect> loadGraphiteRuntimeEffect(std::string_view relativePath) {
  const auto path = paths::assetPath(relativePath);
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("failed to open Graphite SkSL asset: " + path.string());
  }
  const std::string source(std::istreambuf_iterator<char>(stream), {});
  if (!stream.good() && !stream.eof()) {
    throw std::runtime_error("failed to read Graphite SkSL asset: " + path.string());
  }

  auto result = SkRuntimeEffect::MakeForShader(SkString(source));
  if (result.effect == nullptr) {
    throw std::runtime_error(
        "failed to compile Graphite SkSL asset " + path.string() + ": " + result.errorText.c_str()
    );
  }
  return std::move(result.effect);
}

GraphiteRuntimeEffects::GraphiteRuntimeEffects() {
  constexpr std::array<std::string_view, static_cast<std::size_t>(GraphiteRuntimeEffectId::Count)> paths = {
      "shaders/screen_corner.sksl",     "shaders/graph.sksl",          "shaders/fancy_audio_visualizer.sksl",
      "shaders/advanced_rect.sksl",     "shaders/effect_sky.sksl",     "shaders/effect_cloud.sksl",
      "shaders/effect_rain.sksl",       "shaders/effect_snow.sksl",    "shaders/effect_thunder.sksl",
      "shaders/wallpaper_fade.sksl",    "shaders/wallpaper_wipe.sksl", "shaders/wallpaper_disc.sksl",
      "shaders/wallpaper_stripes.sksl", "shaders/wallpaper_zoom.sksl", "shaders/wallpaper_honeycomb.sksl",
  };
  for (std::size_t index = 0; index < paths.size(); ++index) {
    m_effects[index] = loadGraphiteRuntimeEffect(paths[index]);
  }
}
