#include "application.h"
#include "application_internal.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/network/inetwork_service.h"
#include "render/backend/graphite_offscreen_golden.h"
#include "render/backend/graphite_texture_manager.h"
#include "render/backend/render_backend.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <string_view>
#include <vector>

namespace {
  constexpr Logger kLog("app");

  std::string_view powerProfileOriginName(PowerProfilesChangeOrigin origin) {
    switch (origin) {
    case PowerProfilesChangeOrigin::Noctalia:
      return "noctalia";
    case PowerProfilesChangeOrigin::External:
      return "external";
    }
    return "external";
  }
} // namespace

void Application::onIconThemeChanged() {
  kLog.info("system icon theme changed; refreshing icon consumers");
  m_bar.reload();
  m_dock.reload();
  m_panelManager.onIconThemeChanged();
  m_notificationToast.requestLayout();
}

void Application::onGraphiteDeviceLost(RenderDeviceStatus status) {
  if (status != RenderDeviceStatus::Lost) {
    return;
  }
  if (m_graphiteDeviceRecoveryScheduled || m_graphiteDeviceRecoveryAttempted) {
    if (m_graphiteDeviceRecoveryAttempted && !m_graphiteDeviceRecoveryScheduled) {
      kLog.error("Graphite device was lost again after the one permitted recovery attempt");
    }
    return;
  }
  m_graphiteDeviceRecoveryScheduled = true;
  DeferredCall::callLater([this]() { rebuildGraphiteDevice(); });
}

void Application::rebuildGraphiteDevice() {
  if (!m_graphiteDeviceRecoveryScheduled || m_graphiteDeviceRecoveryAttempted) {
    return;
  }
  m_graphiteDeviceRecoveryScheduled = false;
  m_graphiteDeviceRecoveryAttempted = true;
  kLog.warn("rebuilding process-wide Vulkan/Graphite device after VK_ERROR_DEVICE_LOST");

  const GraphicsDeviceIdentity previousIdentity = m_graphicsDevice.identity();
  const bool deviceWasLost = m_graphicsDevice.deviceLost();
  const bool integrationProbe = [] {
    const char* value = std::getenv("NOCTALIA_TEST_GRAPHICS_DEVICE_REBUILD");
    return value != nullptr && std::string_view(value) == "1";
  }();
  std::vector<TextureHandle> staleTextureProbes;
  try {
    if (integrationProbe) {
      auto& probeTextures = m_renderContext.backend().textureManager();
      staleTextureProbes.reserve(32);
      for (std::uint8_t index = 0; index < 32; ++index) {
        const std::array<std::uint8_t, 4> probePixel{
            static_cast<std::uint8_t>(index * 7U), static_cast<std::uint8_t>(255U - index * 5U),
            static_cast<std::uint8_t>(index * 3U), 255
        };
        TextureHandle probe = probeTextures.loadFromRgba(probePixel.data(), 1, 1, false);
        if (!probe.valid()) {
          throw std::runtime_error("device-recovery gate could not create its texture retirement probes");
        }
        staleTextureProbes.push_back(probe);
        if (index % 2U == 0U) {
          // Keep the original ID for the post-rebuild stale-generation check,
          // but retire this allocation before teardown so both queued and
          // still-live manager resources are covered.
          TextureHandle retired = probe;
          probeTextures.unload(retired);
        }
      }
    }
    m_lockScreen.prepareForGraphicsDeviceRebuild();
    m_wallpaper.prepareForGraphicsDeviceRebuild(deviceWasLost);
    m_backdrop.prepareForGraphicsDeviceRebuild(deviceWasLost);
    m_cefService->prepareForGraphicsDeviceRebuild();
    m_sharedTextureCache.abandonGpuResources();
    m_asyncTextureCache.abandonGpuResources();
    m_thumbnailService.abandonGpuResources();
    m_renderContext.prepareForGraphicsDeviceRebuild();
    m_renderContext.cleanup();
    m_graphicsDevice.rebuild();
    const GraphicsDeviceIdentity rebuiltIdentity = m_graphicsDevice.identity();
    if (rebuiltIdentity.uuid != previousIdentity.uuid) {
      throw std::runtime_error(
          "Vulkan recovery selected a different GPU than the running CEF process"
      );
    }
    m_renderContext.initializeGraphite(m_graphicsDevice);
    m_renderContext.setTextFontFamily(m_configService.config().shell.fontFamily);
    auto& textures = m_renderContext.backend().textureManager();
    m_sharedTextureCache.initialize(textures);
    m_sharedTextureCache.reloadResidentTextures();
    m_asyncTextureCache.initialize(textures);
    m_asyncTextureCache.reloadResidentTextures();
    (void)m_thumbnailService.uploadPending(textures);
    if (integrationProbe && !staleTextureProbes.empty()) {
      auto* graphiteTextures = dynamic_cast<GraphiteTextureManager*>(&textures);
      if (graphiteTextures == nullptr) {
        throw std::runtime_error("device recovery did not restore a Graphite texture manager");
      }
      for (const TextureHandle& stale : staleTextureProbes) {
        if (graphiteTextures->image(stale.id) != nullptr) {
          throw std::runtime_error("device recovery allowed a stale texture generation to resolve");
        }
      }
      constexpr std::array<std::uint8_t, 4> replacementPixel{0, 255, 255, 255};
      TextureHandle replacement = textures.loadFromRgba(replacementPixel.data(), 1, 1, false);
      const bool aliasedStale = std::ranges::any_of(staleTextureProbes, [&](const TextureHandle& stale) {
        return replacement.id == stale.id;
      });
      if (!replacement.valid() || aliasedStale) {
        throw std::runtime_error("device recovery reused a stale texture generation");
      }
      textures.unload(replacement);
      runGraphiteOffscreenGolden(m_graphicsDevice);
      kLog.info("device-recovery stale-generation and full Graphite golden passed");
    }
    m_renderContext.resumeAfterGraphicsDeviceRebuild();
    m_cefService->resumeAfterGraphicsDeviceRebuild(m_graphicsDevice);
    m_wallpaper.resumeAfterGraphicsDeviceRebuild();
    m_backdrop.resumeAfterGraphicsDeviceRebuild();
    m_lockScreen.resumeAfterGraphicsDeviceRebuild();
    requestAllSurfacesRedraw();
    kLog.info("Vulkan/Graphite device recovery completed; requested a fresh CEF frame");
  } catch (const std::exception& error) {
    kLog.error("Vulkan/Graphite device recovery failed: {}", error.what());
  }
}

void Application::requestAllSurfacesRedraw() {
  DeferredCall::callLater([this]() {
    m_bar.requestRedraw();
    m_dock.requestRedraw();
    m_desktopWidgetsController.requestRedraw();
    m_panelManager.requestRedraw();
    m_notificationToast.requestRedraw();
    m_osdOverlay.requestRedraw();
    m_lockScreen.requestLayout();
    m_colorPickerDialogPopup.requestRedraw();
    m_glyphPickerDialogPopup.requestRedraw();
    m_fileDialogPopup.requestRedraw();
  });
}

void Application::onUpowerStateChangedForHooks() {
  if (m_upowerService == nullptr) {
    return;
  }
  for (const auto& event : m_batteryHookState.update(m_upowerService->state())) {
    if (event.env.empty()) {
      m_hookManager.fire(event.kind);
    } else {
      m_hookManager.fire(event.kind, event.env);
    }
  }
}

void Application::onNetworkStateChangedForEvents(const NetworkState& state, NetworkChangeOrigin origin) {
  if (!m_prevWirelessEnabledForEvents.has_value()) {
    m_prevWirelessEnabledForEvents = state.wirelessEnabled;
    return;
  }
  const bool prev = *m_prevWirelessEnabledForEvents;
  if (prev != state.wirelessEnabled) {
    if (origin != NetworkChangeOrigin::Noctalia) {
      m_osdOverlay.show(wifiOsdContent(state.wirelessEnabled));
    }
    if (state.wirelessEnabled) {
      m_hookManager.fire(HookKind::WifiEnabled);
    } else {
      m_hookManager.fire(HookKind::WifiDisabled);
    }
  }
  m_prevWirelessEnabledForEvents = state.wirelessEnabled;
}

void Application::onBluetoothStateChangedForEvents(const BluetoothState& state, BluetoothStateChangeOrigin origin) {
  if (!m_prevBluetoothPoweredForEvents.has_value()) {
    m_prevBluetoothPoweredForEvents = state.powered;
    return;
  }
  const bool prev = *m_prevBluetoothPoweredForEvents;
  if (prev != state.powered) {
    if (origin != BluetoothStateChangeOrigin::Noctalia) {
      m_osdOverlay.show(bluetoothOsdContent(state.powered));
    }
    if (state.powered) {
      m_hookManager.fire(HookKind::BluetoothEnabled);
    } else {
      m_hookManager.fire(HookKind::BluetoothDisabled);
    }
  }
  m_prevBluetoothPoweredForEvents = state.powered;
}

void Application::onPowerProfileChangedForEvents(const PowerProfilesState& state, PowerProfilesChangeOrigin origin) {
  if (state.activeProfile.empty()) {
    return;
  }
  if (!m_prevPowerProfileActiveForEvents.has_value()) {
    m_prevPowerProfileActiveForEvents = state.activeProfile;
    return;
  }
  const std::string prev = *m_prevPowerProfileActiveForEvents;
  if (prev != state.activeProfile) {
    if (origin != PowerProfilesChangeOrigin::Noctalia) {
      m_osdOverlay.show(powerProfileOsdContent(state.activeProfile));
    }
    m_hookManager.fire(
        HookKind::PowerProfileChanged,
        {{"NOCTALIA_POWER_PROFILE", state.activeProfile},
         {"NOCTALIA_POWER_PROFILE_PREVIOUS", prev},
         {"NOCTALIA_POWER_PROFILE_ORIGIN", std::string(powerProfileOriginName(origin))}}
    );
  }
  m_prevPowerProfileActiveForEvents = state.activeProfile;
}
