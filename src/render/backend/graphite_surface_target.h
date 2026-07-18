#pragma once

#include "render/presentation_timing.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

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

[[nodiscard]] constexpr RenderFrameStatus classifyVulkanWsiResult(VkResult result) noexcept {
  switch (result) {
  case VK_SUCCESS:
    return RenderFrameStatus::Presented;
  case VK_SUBOPTIMAL_KHR:
  case VK_ERROR_OUT_OF_DATE_KHR:
    return RenderFrameStatus::RecreateSwapchain;
  case VK_ERROR_SURFACE_LOST_KHR:
    return RenderFrameStatus::SurfaceLost;
  case VK_ERROR_DEVICE_LOST:
    return RenderFrameStatus::DeviceLost;
  default:
    return RenderFrameStatus::Failed;
  }
}

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
  [[nodiscard]] RenderFrameStatus endFrame(
      std::span<const VkSemaphore> waitSemaphores = {}, std::span<const VkSemaphore> signalSemaphores = {},
      const std::function<void()>& recordingSubmitted = {}
  );
  // True only for the validation seam which reports device loss after a real,
  // synchronously completed submit. Such a target must use orderly teardown
  // rather than abandoning genuinely lost-device synchronization objects.
  [[nodiscard]] bool deviceLossWasInjected() const noexcept;
  [[nodiscard]] VkFormat format() const noexcept;
  [[nodiscard]] VkExtent2D extent() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
