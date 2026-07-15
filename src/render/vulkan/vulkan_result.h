#pragma once

#include <vulkan/vulkan.h>

#include <string_view>

[[nodiscard]] const char* vulkanResultName(VkResult result) noexcept;
void requireVulkan(VkResult result, std::string_view operation);
