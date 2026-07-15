#include "render/vulkan/vulkan_result.h"

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
}
