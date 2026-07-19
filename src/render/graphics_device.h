#pragma once

#include "render/core/texture_handle.h"

#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct wl_display;
struct wp_presentation;

namespace skgpu::graphite {
  class Context;
  class Recorder;
}

class GraphiteTextureManager;

struct GraphicsDeviceRequirements {
  bool cefExternalMemory = false;
  bool validation = false;
};

struct GraphicsDeviceCandidate {
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  std::uint32_t queueFamily = UINT32_MAX;
  VkPhysicalDeviceProperties properties{};
};

struct GraphicsDeviceIdentity {
  std::array<std::uint8_t, VK_UUID_SIZE> uuid{};
  std::string drmRenderNode;
  std::uint32_t vendorId = 0;
};

// One process-wide Vulkan device and one main-thread Graphite recorder. Vulkan
// WSI remains application-owned; Skia owns allocations made for Graphite
// resources through VulkanAMDMemoryAllocator.
class GraphicsDevice {
public:
  GraphicsDevice();
  ~GraphicsDevice();

  GraphicsDevice(const GraphicsDevice&) = delete;
  GraphicsDevice& operator=(const GraphicsDevice&) = delete;

  void initialize(
      wl_display* display, GraphicsDeviceRequirements requirements = {},
      wp_presentation* presentation = nullptr, std::int32_t presentationClockId = -1
  );
  void cleanup();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] VkInstance instance() const noexcept { return m_instance; }
  [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept { return m_physicalDevice; }
  [[nodiscard]] VkDevice device() const noexcept { return m_device; }
  [[nodiscard]] VkQueue graphicsQueue() const noexcept { return m_graphicsQueue; }
  [[nodiscard]] std::uint32_t graphicsQueueFamily() const noexcept { return m_graphicsQueueFamily; }
  [[nodiscard]] wl_display* waylandDisplay() const noexcept { return m_waylandDisplay; }
  [[nodiscard]] wp_presentation* waylandPresentation() const noexcept { return m_waylandPresentation; }
  [[nodiscard]] std::int32_t presentationClockId() const noexcept { return m_presentationClockId; }
  [[nodiscard]] skgpu::graphite::Context* graphiteContext() const noexcept;
  [[nodiscard]] skgpu::graphite::Recorder* recorder() const noexcept;
  // Device-loss teardown still has to destroy Noctalia-owned backend textures.
  // This accessor deliberately remains available after valid() becomes false;
  // callers must not record or submit work through the returned context.
  [[nodiscard]] skgpu::graphite::Context* graphiteContextForResourceDestruction() const noexcept;
  [[nodiscard]] GraphiteTextureManager& textureManager();
  [[nodiscard]] const std::vector<std::string>& instanceExtensions() const noexcept {
    return m_instanceExtensions;
  }
  [[nodiscard]] const std::vector<std::string>& deviceExtensions() const noexcept {
    return m_deviceExtensions;
  }
  [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }
  [[nodiscard]] std::optional<std::uint32_t> allocateTextureGeneration() noexcept {
    return m_textureGenerations.next();
  }
  [[nodiscard]] bool cefExternalMemoryEnabled() const noexcept { return m_cefExternalMemoryEnabled; }
  [[nodiscard]] bool validationEnabled() const noexcept { return m_requirements.validation; }
  [[nodiscard]] bool deviceLost() const noexcept { return m_deviceLost; }
  [[nodiscard]] GraphicsDeviceIdentity identity() const;
  [[nodiscard]] std::uint64_t validationErrorCount() const noexcept {
    return m_validationMessages[0].load();
  }
  [[nodiscard]] std::uint64_t validationWarningCount() const noexcept {
    return m_validationMessages[1].load();
  }

  // Marks the current logical device unusable, detaches Graphite references,
  // and destroys owned backend textures without submitting cleanup work.
  // Remaining Vulkan handles are destroyed by rebuild().
  void abandonAfterDeviceLoss() noexcept;

  // Drops all Graphite/Vulkan objects, recreates the same contract, and bumps
  // the generation so every opaque TextureHandle becomes stale.
  void rebuild();

  [[nodiscard]] static bool supportsDeviceExtensions(
      VkPhysicalDevice device, std::span<const char* const> extensions
  );

private:
  struct GraphiteState;

  void createInstance(bool validation);
  void selectPhysicalDevice();
  void createLogicalDevice();
  void createGraphite();
  void destroyDeviceObjects();
  void destroyInstanceObjects();
  void configureExtensionContract();

  wl_display* m_waylandDisplay = nullptr;
  wp_presentation* m_waylandPresentation = nullptr;
  std::int32_t m_presentationClockId = -1;
  GraphicsDeviceRequirements m_requirements;
  VkInstance m_instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
  VkDevice m_device = VK_NULL_HANDLE;
  VkQueue m_graphicsQueue = VK_NULL_HANDLE;
  std::uint32_t m_graphicsQueueFamily = UINT32_MAX;
  std::vector<std::string> m_instanceExtensions;
  std::vector<std::string> m_deviceExtensions;
  std::unique_ptr<GraphiteState> m_graphite;
  std::unique_ptr<GraphiteTextureManager> m_textureManager;
  std::uint64_t m_generation = 1;
  TextureGenerationAllocator m_textureGenerations;
  bool m_cefExternalMemoryEnabled = false;
  bool m_deviceLost = false;
  // [0] errors, [1] warnings. Passed directly to the debug-utils callback so
  // the phase-2 gate can make validation errors fatal.
  std::array<std::atomic<std::uint64_t>, 2> m_validationMessages{};
};
