#pragma once

#include "render/backend/graphite_texture_manager.h"
#include "render/core/texture_handle.h"

#include <vulkan/vulkan.h>

#include "include/core/SkRefCnt.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

class GraphicsDevice;
class SkImage;

struct BorrowedDmabufPlane {
  int fd = -1;
  std::uint32_t stride = 0;
  std::uint64_t offset = 0;
};

// The file descriptors in this value are borrowed from CEF and are valid only
// for the duration of CefGpuFrameBridge::acceptFrame(). The bridge never stores
// or closes them.
struct BorrowedDmabufFrame {
  std::int64_t captureCounter = -1;
  int width = 0;
  int height = 0;
  std::uint32_t fourcc = 0;
  std::uint64_t modifier = 0;
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
  std::uint64_t peakImports = 0;
  std::uint64_t importCacheHits = 0;
  std::uint64_t importCacheMisses = 0;
  std::uint64_t importCacheEvictions = 0;
  std::uint64_t dmabufFdsDuplicated = 0;
  std::uint64_t dmabufFdsTransferred = 0;
  std::uint64_t dmabufFdsClosed = 0;
  std::uint64_t acquireSemaphoresCreated = 0;
  std::uint64_t acquireSemaphoresDestroyed = 0;
  std::uint64_t activeAcquireSemaphores = 0;
  std::uint64_t peakAcquireSemaphores = 0;
  std::uint64_t releaseSemaphoresCreated = 0;
  std::uint64_t releaseSemaphoresDestroyed = 0;
  std::uint64_t releaseFenceFdsExported = 0;
  std::uint64_t completionTimelinesCreated = 0;
  std::uint64_t completionTimelinesDestroyed = 0;
  std::uint64_t completionValuesSubmitted = 0;
  std::uint64_t completionValuesWaited = 0;
  std::uint64_t importCompletionWaits = 0;
  std::uint64_t fenceFdsDuplicated = 0;
  std::uint64_t fenceFdsTransferred = 0;
  std::uint64_t fenceFdsClosed = 0;
};

// CEF DMA-BUF/Graphite bridge. The production path caches modifier-backed
// Vulkan imports, wraps them as Graphite images, and samples them directly. A
// token-correlated release fence delays CEF buffer recycling until the Graphite
// sampling submission has consumed the image. The cache owns only
// duplicated/imported handles; BorrowedDmabufFrame FDs are neither stored nor
// closed. There is no copy or CPU-rendering fallback.
class CefGpuFrameBridge final : public GraphiteExternalImageSynchronization {
public:
  using ReleaseFenceCallback = std::function<void(std::int64_t captureCounter, int fd)>;

  explicit CefGpuFrameBridge(
      GraphicsDevice& graphics, GraphiteTextureManager* textures = nullptr,
      ReleaseFenceCallback releaseFenceCallback = {}
  );
  ~CefGpuFrameBridge();

  CefGpuFrameBridge(const CefGpuFrameBridge&) = delete;
  CefGpuFrameBridge& operator=(const CefGpuFrameBridge&) = delete;

  [[nodiscard]] bool acceptFrame(const BorrowedDmabufFrame& frame);
  // Transfers the sync FD signaled after the most recent imported image use.
  [[nodiscard]] int takeReleaseFenceFd() noexcept;
  [[nodiscard]] bool prepareForGraphiteSampling() override;
  void releaseAfterGraphiteSampling() override;
  // Balances and releases a frame that will not be drawn (for example when a
  // panel is detached after CEF has delivered a paint).
  void discardPendingFrame();
  // Lost-device teardown: drop pending ownership without queue waits/submits.
  void abandonDevice() noexcept;
  void invalidate();

  [[nodiscard]] SkImage* image() const noexcept;
  [[nodiscard]] TextureHandle texture() const noexcept;
  [[nodiscard]] int width() const noexcept;
  [[nodiscard]] int height() const noexcept;
  [[nodiscard]] std::uint64_t acceptedFrameCount() const noexcept;
  [[nodiscard]] CefGpuFrameBridgeStats stats() const noexcept;
  [[nodiscard]] const std::string& lastError() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
