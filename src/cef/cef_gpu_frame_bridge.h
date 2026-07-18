#pragma once

#include "render/backend/graphite_texture_manager.h"
#include "render/core/texture_handle.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vulkan/vulkan.h>

class GraphicsDevice;

struct BorrowedDmabufPlane {
  int fd = -1;
  std::uint32_t stride = 0;
  std::uint64_t offset = 0;
};

// The file descriptors in this value are borrowed from CEF and are valid only
// for the duration of CefGpuFrameBridge::acceptFrame(). The bridge never stores
// or closes them.
struct BorrowedDmabufFrame {
  std::uint64_t transportEpoch = 0;
  std::int64_t captureCounter = -1;
  std::uint64_t outputGeneration = 0;
  std::uint32_t outputSlot = 0;
  std::uint64_t contentSerial = 0;
  int width = 0;
  int height = 0;
  std::uint32_t fourcc = 0;
  std::uint64_t modifier = 0;
  std::uint32_t queueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  std::uint32_t imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  int acquireFenceFd = -1;
  int planeCount = 0;
  std::array<BorrowedDmabufPlane, 4> planes{};
};

struct CefGpuFrameBridgeStats {
  std::uint64_t framesAccepted = 0;
  std::uint64_t directFramesStaged = 0;
  std::uint64_t directFramesSampled = 0;
  std::uint64_t directFramesDiscarded = 0;
  std::uint64_t importsCreated = 0;
  std::uint64_t importsDestroyed = 0;
  std::uint64_t activeImports = 0;
  std::uint64_t importCacheHits = 0;
  std::uint64_t importCacheMisses = 0;
  std::uint64_t importCacheEvictions = 0;
  std::uint64_t releaseFenceFdsExported = 0;
};

// CEF DMA-BUF/Graphite bridge. The production path caches modifier-backed
// Vulkan imports, wraps them as Graphite images, and samples them directly. A
// token-correlated release fence delays CEF buffer recycling until the Graphite
// sampling submission has consumed the image. The cache owns only
// duplicated/imported handles; BorrowedDmabufFrame FDs are neither stored nor
// closed. There is no copy or CPU-rendering fallback.
class CefGpuFrameBridge final : public GraphiteExternalImageSynchronization {
public:
  using ReleaseFenceCallback = std::function<void(std::uint64_t transportEpoch, std::int64_t captureCounter, int fd)>;

  CefGpuFrameBridge(
      GraphicsDevice& graphics, GraphiteTextureManager& textures, ReleaseFenceCallback releaseFenceCallback
  );
  ~CefGpuFrameBridge();

  CefGpuFrameBridge(const CefGpuFrameBridge&) = delete;
  CefGpuFrameBridge& operator=(const CefGpuFrameBridge&) = delete;

  [[nodiscard]] bool acceptFrame(const BorrowedDmabufFrame& frame) noexcept;
  [[nodiscard]] bool prepareForGraphiteSampling(GraphiteSubmissionDependency& dependency) override;
  void finishGraphiteSampling(const GraphiteSubmissionDependency& dependency, bool submitted) override;
  // Balances and releases a frame that will not be drawn (for example when a
  // panel is detached after CEF has delivered a paint).
  void discardPendingFrame();
  // Lost-device teardown: drop pending ownership without queue waits/submits.
  void abandonDevice() noexcept;
  void invalidate();

  [[nodiscard]] TextureHandle texture() const noexcept;
  [[nodiscard]] CefGpuFrameBridgeStats stats() const noexcept;
  [[nodiscard]] const std::string& lastError() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
