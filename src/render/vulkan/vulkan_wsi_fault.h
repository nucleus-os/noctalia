#pragma once

#include <vulkan/vulkan.h>

#include <optional>
#include <string_view>

enum class VulkanWsiCall { Acquire, Present, GraphiteSubmit };

struct VulkanWsiFault {
  VulkanWsiCall call = VulkanWsiCall::Acquire;
  VkResult result = VK_SUCCESS;
  // Acquire errors which return no image are injected without calling WSI.
  // Results that require a real acquired/presented image override a successful
  // driver result after the call, preserving synchronization semantics.
  bool skipDriverCall = false;
};

[[nodiscard]] std::optional<VulkanWsiFault> parseVulkanWsiFault(std::string_view value) noexcept;

// Consumes NOCTALIA_TEST_VULKAN_WSI_FAULT once process-wide. Production builds
// pay only an environment lookup and an atomic branch until the optional test
// fault has fired.
[[nodiscard]] std::optional<VulkanWsiFault> takeInjectedVulkanWsiFault(VulkanWsiCall call) noexcept;
