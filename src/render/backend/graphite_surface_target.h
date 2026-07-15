#pragma once

#include "render/presentation_timing.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>

class GraphicsDevice;
class SkCanvas;
struct wl_surface;

enum class RenderFrameStatus : std::uint8_t {
  Presented,
  Deferred,
  RecreateSwapchain,
  SurfaceLost,
  DeviceLost,
  Failed,
};

// Vulkan/Wayland WSI for one wl_surface. Every surface has an independent
// FIFO swapchain while drawing and resource allocation share GraphicsDevice's
// one Graphite recorder.
class GraphiteSurfaceTarget {
public:
  GraphiteSurfaceTarget(GraphicsDevice& graphics, wl_surface* surface);
  ~GraphiteSurfaceTarget();

  GraphiteSurfaceTarget(const GraphiteSurfaceTarget&) = delete;
  GraphiteSurfaceTarget& operator=(const GraphiteSurfaceTarget&) = delete;

  void resize(std::uint32_t width, std::uint32_t height);
  void setPresentationCallback(SurfacePresentationCallback callback);
  // Marks all in-flight work as irrecoverable after VK_ERROR_DEVICE_LOST so
  // teardown destroys handles without waiting on fences that can never signal.
  void abandonDevice() noexcept;
  void destroy();

  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] SkCanvas* beginFrame(RenderFrameStatus& status);
  [[nodiscard]] RenderFrameStatus endFrame(const std::function<void()>& recordingSubmitted = {});
  [[nodiscard]] VkFormat format() const noexcept;
  [[nodiscard]] VkExtent2D extent() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
