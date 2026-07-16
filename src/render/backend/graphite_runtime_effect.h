#pragma once

#include "include/effects/SkRuntimeEffect.h"

#include <array>
#include <cstddef>
#include <string_view>

// Load and compile an SkSL shader from Noctalia's source/install asset bundle.
// Throws a named startup error containing the asset path and SkSL diagnostics.
[[nodiscard]] sk_sp<SkRuntimeEffect> loadGraphiteRuntimeEffect(std::string_view assetPath);

enum class GraphiteRuntimeEffectId : std::size_t {
  ScreenCorner,
  Graph,
  FancyAudioVisualizer,
  AdvancedRect,
  WeatherSky,
  WeatherCloud,
  WeatherRain,
  WeatherSnow,
  WeatherThunder,
  WallpaperFade,
  WallpaperWipe,
  WallpaperDisc,
  WallpaperStripes,
  WallpaperZoom,
  WallpaperHoneycomb,
  Count,
};

// Process-local, immutable registry. Construction compiles the entire effect
// family so a missing or invalid shader is reported before the first frame.
class GraphiteRuntimeEffects {
public:
  GraphiteRuntimeEffects();

  [[nodiscard]] const sk_sp<SkRuntimeEffect>& get(GraphiteRuntimeEffectId id) const noexcept {
    return m_effects[static_cast<std::size_t>(id)];
  }

private:
  std::array<sk_sp<SkRuntimeEffect>, static_cast<std::size_t>(GraphiteRuntimeEffectId::Count)> m_effects;
};
