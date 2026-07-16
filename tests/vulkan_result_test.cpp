#include "render/vulkan/vulkan_result.h"
#include "render/vulkan/vulkan_wsi_fault.h"
#include "render/backend/graphite_surface_target.h"

#include <cassert>
#include <stdexcept>
#include <string>

int main() {
  try {
    requireVulkan(VK_ERROR_OUT_OF_DATE_KHR, "vkAcquireNextImageKHR");
    assert(false && "requireVulkan must throw for an error");
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    assert(message.contains("vkAcquireNextImageKHR"));
    assert(message.contains("VK_ERROR_OUT_OF_DATE_KHR"));
  }
  requireVulkan(VK_SUCCESS, "success");

  assert(std::string(vulkanResultName(VK_SUBOPTIMAL_KHR)) == "VK_SUBOPTIMAL_KHR");
  assert(std::string(vulkanResultName(static_cast<VkResult>(123456))) == "VK_RESULT_UNKNOWN");

  const auto acquireOutOfDate = parseVulkanWsiFault("acquire-out-of-date");
  assert(acquireOutOfDate.has_value());
  assert(acquireOutOfDate->call == VulkanWsiCall::Acquire);
  assert(acquireOutOfDate->result == VK_ERROR_OUT_OF_DATE_KHR);
  assert(acquireOutOfDate->skipDriverCall);

  const auto acquireSuboptimal = parseVulkanWsiFault("acquire-suboptimal");
  assert(acquireSuboptimal.has_value());
  assert(acquireSuboptimal->result == VK_SUBOPTIMAL_KHR);
  assert(!acquireSuboptimal->skipDriverCall);

  const auto surfaceLost = parseVulkanWsiFault("present-surface-lost");
  assert(surfaceLost.has_value());
  assert(surfaceLost->call == VulkanWsiCall::Present);
  assert(surfaceLost->result == VK_ERROR_SURFACE_LOST_KHR);
  assert(!surfaceLost->skipDriverCall);

  const auto submitFailure = parseVulkanWsiFault("graphite-submit-failure");
  assert(submitFailure.has_value());
  assert(submitFailure->call == VulkanWsiCall::GraphiteSubmit);
  assert(submitFailure->result == VK_ERROR_DEVICE_LOST);
  assert(!submitFailure->skipDriverCall);

  assert(!parseVulkanWsiFault("device-lost"));
  assert(!parseVulkanWsiFault("unknown"));

  static_assert(classifyVulkanWsiResult(VK_SUCCESS) == RenderFrameStatus::Presented);
  static_assert(classifyVulkanWsiResult(VK_SUBOPTIMAL_KHR) == RenderFrameStatus::RecreateSwapchain);
  static_assert(classifyVulkanWsiResult(VK_ERROR_OUT_OF_DATE_KHR) == RenderFrameStatus::RecreateSwapchain);
  static_assert(classifyVulkanWsiResult(VK_ERROR_SURFACE_LOST_KHR) == RenderFrameStatus::SurfaceLost);
  static_assert(classifyVulkanWsiResult(VK_ERROR_DEVICE_LOST) == RenderFrameStatus::DeviceLost);
  static_assert(classifyVulkanWsiResult(VK_ERROR_UNKNOWN) == RenderFrameStatus::Failed);
}
