#pragma once

#include "include/core/SkRefCnt.h"
#include "render/backend/graphite_resource_observer.h"
#include "render/core/texture_manager.h"

#include <vulkan/vulkan.h>

#include <memory>

class GraphicsDevice;
class SkImage;

struct GraphiteSubmissionDependency {
  VkSemaphore waitSemaphore = VK_NULL_HANDLE;
  VkSemaphore signalSemaphore = VK_NULL_HANDLE;
};

// Synchronizes an externally-owned Vulkan image with the Graphite queue. The
// prepare call happens only for a frame that actually records a draw using the
// image. Its semaphores are attached to that exact Graphite recording.
class GraphiteExternalImageSynchronization {
public:
  virtual ~GraphiteExternalImageSynchronization() = default;
  [[nodiscard]] virtual bool prepareForGraphiteSampling(GraphiteSubmissionDependency& dependency) = 0;
  // Called after the recording that referenced the image either reached
  // Graphite's submission queue or failed before submission.
  virtual void finishGraphiteSampling(const GraphiteSubmissionDependency& dependency, bool submitted) = 0;
};

class GraphiteTextureManager final : public TextureManager {
public:
  explicit GraphiteTextureManager(GraphicsDevice& graphics);
  ~GraphiteTextureManager() override;

  [[nodiscard]] TextureHandle loadFromFile(const std::string& path, int targetSize = 0, bool mipmap = false) override;
  [[nodiscard]] TextureHandle
  loadFromEncodedBytes(const std::uint8_t* data, std::size_t size, bool mipmap = false) override;
  [[nodiscard]] TextureHandle
  loadFromRgba(const std::uint8_t* data, int width, int height, bool mipmap = false) override;
  [[nodiscard]] TextureHandle loadFromRaw(
      const std::uint8_t* data, std::size_t size, int width, int height, int stride, PixmapFormat format,
      bool mipmap = false
  ) override;
  [[nodiscard]] TextureHandle loadFromPixels(
      const std::uint8_t* data, int width, int height, TextureDataFormat format,
      TextureFilter filter = TextureFilter::Linear, bool mipmap = false
  ) override;
  [[nodiscard]] TextureHandle
  createEmpty(int width, int height, TextureDataFormat format, TextureFilter filter = TextureFilter::Linear) override;
  [[nodiscard]] TextureHandle createBgraSurface(int width, int height) override;
  bool updateBgraSurface(TextureHandle& handle, const std::uint8_t* data, int width, int height) override;
  bool replace(
      TextureHandle& handle, const std::uint8_t* data, int width, int height, TextureDataFormat format,
      TextureFilter filter = TextureFilter::Linear, bool mipmap = false
  ) override;
  bool updateSubImage(
      TextureHandle& handle, const std::uint8_t* data, int x, int y, int width, int height, TextureDataFormat format
  ) override;
  void unload(TextureHandle& handle) override;
  void cleanup() override;
  void abandonGpuResources() noexcept override;

  [[nodiscard]] SkImage* image(TextureId id) const noexcept;
  [[nodiscard]] TextureFilter filter(TextureId id) const noexcept;
  [[nodiscard]] TextureHandle adoptExternalImage(
      sk_sp<SkImage> image, int width, int height, TextureFilter filter = TextureFilter::Linear,
      GraphiteExternalImageSynchronization* synchronization = nullptr
  );
  // Replaces the image behind an externally-owned texture without changing its
  // TextureId generation. This is intentionally restricted to entries created
  // by adoptExternalImage(); GraphiteTextureManager never assumes ownership of
  // the external BackendTexture.
  bool rebindExternalImage(TextureHandle& handle, sk_sp<SkImage> image, int width, int height);
  [[nodiscard]] GraphiteExternalImageSynchronization* externalSynchronization(TextureId id) const noexcept;
  void addObserver(GraphiteTextureManagerObserver& observer);
  void removeObserver(GraphiteTextureManagerObserver& observer) noexcept;
  void invalidateAll();

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
