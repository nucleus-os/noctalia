#include "render/vulkan/vulkan_wsi_fault.h"

#include <atomic>
#include <cstdlib>
#include <string_view>

std::optional<VulkanWsiFault> parseVulkanWsiFault(std::string_view value) noexcept {
  if (value == "acquire-out-of-date") {
    return VulkanWsiFault{VulkanWsiCall::Acquire, VK_ERROR_OUT_OF_DATE_KHR, true};
  }
  if (value == "acquire-suboptimal") {
    return VulkanWsiFault{VulkanWsiCall::Acquire, VK_SUBOPTIMAL_KHR, false};
  }
  if (value == "acquire-surface-lost") {
    return VulkanWsiFault{VulkanWsiCall::Acquire, VK_ERROR_SURFACE_LOST_KHR, true};
  }
  if (value == "present-out-of-date") {
    return VulkanWsiFault{VulkanWsiCall::Present, VK_ERROR_OUT_OF_DATE_KHR, false};
  }
  if (value == "present-suboptimal") {
    return VulkanWsiFault{VulkanWsiCall::Present, VK_SUBOPTIMAL_KHR, false};
  }
  if (value == "present-surface-lost") {
    return VulkanWsiFault{VulkanWsiCall::Present, VK_ERROR_SURFACE_LOST_KHR, false};
  }
  if (value == "graphite-submit-failure") {
    return VulkanWsiFault{VulkanWsiCall::GraphiteSubmit, VK_ERROR_DEVICE_LOST, false};
  }
  return std::nullopt;
}

std::optional<VulkanWsiFault> takeInjectedVulkanWsiFault(VulkanWsiCall call) noexcept {
  static const std::optional<VulkanWsiFault> configured = [] {
    const char* value = std::getenv("NOCTALIA_TEST_VULKAN_WSI_FAULT");
    return value != nullptr ? parseVulkanWsiFault(value) : std::nullopt;
  }();
  static std::atomic_bool consumed = false;
  if (!configured.has_value() || configured->call != call || consumed.exchange(true, std::memory_order_relaxed)) {
    return std::nullopt;
  }
  return configured;
}
