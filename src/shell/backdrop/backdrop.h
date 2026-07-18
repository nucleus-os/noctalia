#pragma once

#include "config/config_types.h"

#include <memory>
#include <string>
#include <vector>

struct BackdropInstance;
class ConfigService;
class GraphicsDevice;
class WaylandConnection;
struct WaylandOutput;

class Backdrop {
public:
  Backdrop();
  ~Backdrop();

  bool initialize(WaylandConnection& wayland, ConfigService* config, GraphicsDevice& graphics);
  void onOutputChange();
  void onFontChanged();
  void onStateChange();
  void onThemeChanged();
  void prepareForGraphicsDeviceRebuild(bool deviceLost);
  void resumeAfterGraphicsDeviceRebuild();
  void requestLayout();

private:
  [[nodiscard]] bool isSupportedForCurrentCompositor() const;
  [[nodiscard]] bool shouldHaveInstances() const;
  void reload();
  void cacheReloadBaseline();
  void destroyInstances();
  void abandonInstancesAfterDeviceLoss() noexcept;
  void syncInstances();
  void createInstance(const WaylandOutput& output);
  void loadWallpaper(BackdropInstance& inst, const std::string& path);
  void updateRendererState(BackdropInstance& inst);
  void releaseInstanceTexture(BackdropInstance& inst, bool clearPath = true);

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  GraphicsDevice* m_graphics = nullptr;
  BackdropConfig m_lastBackdropConfig{};
  bool m_lastShouldHaveInstances = false;
  bool m_lastWallpaperEnabled = true;
  WallpaperFillMode m_lastWallpaperFillMode = WallpaperFillMode::Crop;
  std::vector<std::unique_ptr<BackdropInstance>> m_instances;
};
