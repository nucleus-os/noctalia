#include "render/backend/graphite_surface_target.h"

#include "core/log.h"
#include "core/tracy.h"
#include "core/tracy_latency.h"
#include "render/graphics_device.h"
#include "render/vulkan/vulkan_result.h"
#include "render/vulkan/vulkan_wsi_fault.h"

#include <wayland-client-core.h>
#include <vulkan/vulkan_wayland.h>

#include "presentation-time-client-protocol.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSurface.h"
#include "include/gpu/MutableTextureState.h"
#include "include/gpu/graphite/BackendSemaphore.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Recording.h"
#include "include/gpu/graphite/Surface.h"
#include "include/gpu/graphite/vk/VulkanGraphiteTypes.h"
#include "include/gpu/vk/VulkanMutableTextureState.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
  constexpr Logger kLog("vulkan-wsi");
  constexpr std::size_t kFramesInFlight = 2;

  VkSurfaceFormatKHR chooseSurfaceFormat(std::span<const VkSurfaceFormatKHR> formats) {
    constexpr VkFormat preferred[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    for (VkFormat format : preferred) {
      const auto found = std::ranges::find_if(formats, [format](const VkSurfaceFormatKHR& candidate) {
        return candidate.format == format && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      });
      if (found != formats.end()) {
        return *found;
      }
    }
    throw std::runtime_error(
        "Wayland surface supports neither B8G8R8A8_UNORM nor R8G8B8A8_UNORM with SRGB_NONLINEAR"
    );
  }

  const char* surfaceFormatName(VkFormat format) {
    switch (format) {
      case VK_FORMAT_B8G8R8A8_SRGB: return "BGRA8_SRGB";
      case VK_FORMAT_R8G8B8A8_SRGB: return "RGBA8_SRGB";
      case VK_FORMAT_B8G8R8A8_UNORM: return "BGRA8_UNORM";
      case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8_UNORM";
      default: return "unknown";
    }
  }

  VkExtent2D chooseExtent(
      const VkSurfaceCapabilitiesKHR& capabilities, std::uint32_t requestedWidth, std::uint32_t requestedHeight
  ) {
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
      return capabilities.currentExtent;
    }
    return {
        .width = std::clamp(requestedWidth, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
        .height = std::clamp(requestedHeight, capabilities.minImageExtent.height, capabilities.maxImageExtent.height),
    };
  }
}

struct GraphiteSurfaceTarget::Impl {
  enum class SwapchainReason : std::int64_t {
    Initial = 1,
    Resize = 2,
    AcquireOutOfDate = 3,
    AcquireSuboptimal = 4,
    PresentOutOfDate = 5,
    PresentSuboptimal = 6,
  };

  struct SwapchainImage {
    VkImage image = VK_NULL_HANDLE;
    skgpu::graphite::BackendTexture backendTexture;
    sk_sp<SkSurface> surface;
    VkSemaphore presentSemaphore = VK_NULL_HANDLE;
    VkFence presentationFence = VK_NULL_HANDLE;
    bool presentationPending = false;
  };

  struct Generation {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<SwapchainImage> images;
  };

  struct FrameSlot {
    VkFence acquireFence = VK_NULL_HANDLE;
    bool acquirePending = false;
    std::uint32_t acquiredImage = 0;
    std::optional<std::uint32_t> lastImage;
  };

  struct PresentationFeedback {
    Impl* owner = nullptr;
    struct wp_presentation_feedback* proxy = nullptr;
    tracy_latency::PresentationTrace trace;
  };

  GraphicsDevice& graphics;
  wl_surface* waylandSurface = nullptr;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  std::array<FrameSlot, kFramesInFlight> frames;
  std::unique_ptr<Generation> generation;
  std::vector<std::unique_ptr<Generation>> retired;
  std::size_t nextFrame = 0;
  FrameSlot* currentFrame = nullptr;
  SwapchainImage* currentImage = nullptr;
  std::uint32_t currentImageIndex = 0;
  std::uint32_t requestedWidth = 0;
  std::uint32_t requestedHeight = 0;
  bool swapchainInvalid = false;
  bool surfaceInvalid = false;
  bool deviceLost = false;
  bool injectedDeviceLoss = false;
  SwapchainReason swapchainReason = SwapchainReason::Initial;
  std::uint64_t swapchainsCreated = 0;
  std::list<PresentationFeedback> presentationFeedbacks;
  std::uint64_t presentationsReported = 0;
  std::uint64_t presentationsDiscarded = 0;
  bool loggedPresentationFeedback = false;
  SurfacePresentationCallback presentationCallback;

  explicit Impl(GraphicsDevice& device, wl_surface* wlSurface) : graphics(device), waylandSurface(wlSurface) {}

  static std::int64_t steadyNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    ).count();
  }

  static void feedbackSyncOutput(
      void* /*data*/, struct wp_presentation_feedback* /*feedback*/, wl_output* /*output*/
  ) {}

  static void feedbackPresented(
      void* data, struct wp_presentation_feedback* /*proxy*/, std::uint32_t tvSecHi, std::uint32_t tvSecLo,
      std::uint32_t tvNsec, std::uint32_t refresh, std::uint32_t sequenceHi, std::uint32_t sequenceLo,
      std::uint32_t flags
  ) {
    auto* feedback = static_cast<PresentationFeedback*>(data);
    feedback->owner->handlePresentation(*feedback, tvSecHi, tvSecLo, tvNsec, refresh, sequenceHi, sequenceLo, flags);
  }

  static void feedbackDiscarded(void* data, struct wp_presentation_feedback* /*proxy*/) {
    auto* feedback = static_cast<PresentationFeedback*>(data);
    feedback->owner->handleDiscarded(*feedback);
  }

  inline static const wp_presentation_feedback_listener kPresentationFeedbackListener = {
      .sync_output = &feedbackSyncOutput,
      .presented = &feedbackPresented,
      .discarded = &feedbackDiscarded,
  };

  PresentationFeedback* requestPresentationFeedback() {
    if (graphics.waylandPresentation() == nullptr) {
      return nullptr;
    }
    struct wp_presentation_feedback* proxy = wp_presentation_feedback(
        graphics.waylandPresentation(), waylandSurface
    );
    if (proxy == nullptr) {
      return nullptr;
    }
    presentationFeedbacks.push_back({.owner = this, .proxy = proxy, .trace = {}});
    PresentationFeedback& feedback = presentationFeedbacks.back();
    if (wp_presentation_feedback_add_listener(proxy, &kPresentationFeedbackListener, &feedback) != 0) {
      wp_presentation_feedback_destroy(proxy);
      presentationFeedbacks.pop_back();
      return nullptr;
    }
    NOCTALIA_TRACE_PLOT(
        "Wayland presentation feedback pending",
        static_cast<std::int64_t>(presentationFeedbacks.size())
    );
    return &feedback;
  }

  void removePresentationFeedback(PresentationFeedback& feedback, bool serverDestroyed) {
    const auto found = std::ranges::find_if(presentationFeedbacks, [&feedback](const PresentationFeedback& candidate) {
      return &candidate == &feedback;
    });
    if (found == presentationFeedbacks.end()) {
      return;
    }
    if (!serverDestroyed && found->proxy != nullptr) {
      wp_presentation_feedback_destroy(found->proxy);
    }
    found->proxy = nullptr;
    presentationFeedbacks.erase(found);
    NOCTALIA_TRACE_PLOT(
        "Wayland presentation feedback pending",
        static_cast<std::int64_t>(presentationFeedbacks.size())
    );
  }

  void handlePresentation(
      PresentationFeedback& feedback, std::uint32_t tvSecHi, std::uint32_t tvSecLo,
      std::uint32_t tvNsec, std::uint32_t refresh, std::uint32_t sequenceHi,
      std::uint32_t sequenceLo, std::uint32_t flags
  ) {
    const std::int64_t callbackSteadyNs = steadyNowNs();
    std::int64_t presentedSteadyNs = callbackSteadyNs;
    bool exactClock = false;
    const std::int32_t clockId = graphics.presentationClockId();
    timespec clockNow{};
    if (clockId >= 0 && tvNsec < 1'000'000'000U
        && clock_gettime(static_cast<clockid_t>(clockId), &clockNow) == 0) {
      const std::uint64_t seconds = (static_cast<std::uint64_t>(tvSecHi) << 32U) | tvSecLo;
      constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
      if (seconds <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / kNanosecondsPerSecond) {
        const std::int64_t protocolNs = static_cast<std::int64_t>(seconds * kNanosecondsPerSecond + tvNsec);
        const std::int64_t clockNowNs = static_cast<std::int64_t>(clockNow.tv_sec) * 1'000'000'000LL
            + static_cast<std::int64_t>(clockNow.tv_nsec);
        presentedSteadyNs = callbackSteadyNs - (clockNowNs - protocolNs);
        exactClock = true;
      }
    }
    const std::int64_t deliveryNs = std::max<std::int64_t>(0, callbackSteadyNs - presentedSteadyNs);
    [[maybe_unused]] const std::uint64_t sequence =
        (static_cast<std::uint64_t>(sequenceHi) << 32U) | sequenceLo;
    ++presentationsReported;
    NOCTALIA_TRACE_PLOT("Wayland presentation sequence", static_cast<std::int64_t>(sequence));
    NOCTALIA_TRACE_PLOT("Wayland presentation flags", static_cast<std::int64_t>(flags));
    NOCTALIA_TRACE_PLOT("Wayland presentation exact clock", static_cast<std::int64_t>(exactClock ? 1 : 0));
    NOCTALIA_TRACE_PLOT("Wayland presentations reported", static_cast<std::int64_t>(presentationsReported));
    tracy_latency::compositorPresented(feedback.trace, presentedSteadyNs, refresh, deliveryNs);
    if (presentationCallback) {
      presentationCallback({
          .presentedSteadyNs = presentedSteadyNs,
          .callbackSteadyNs = callbackSteadyNs,
          .sequence = sequence,
          .refreshNs = refresh,
          .flags = flags,
          .presented = true,
          .exactClock = exactClock,
      });
    }
    if (!loggedPresentationFeedback) {
      loggedPresentationFeedback = true;
      kLog.info(
          "presentation feedback active: refresh={}ns clock={} exact={} flags=0x{:x}",
          refresh, clockId, exactClock ? "yes" : "no", flags
      );
    }
    // Both terminal feedback events are protocol destructors; libwayland has
    // already invalidated the proxy when this callback runs.
    removePresentationFeedback(feedback, true);
  }

  void handleDiscarded(PresentationFeedback& feedback) {
    ++presentationsDiscarded;
    NOCTALIA_TRACE_PLOT("Wayland presentations discarded", static_cast<std::int64_t>(presentationsDiscarded));
    tracy_latency::compositorDiscarded(feedback.trace);
    if (presentationCallback) {
      presentationCallback({.callbackSteadyNs = steadyNowNs(), .presented = false});
    }
    removePresentationFeedback(feedback, true);
  }

  void cancelPresentationFeedback(PresentationFeedback* feedback) {
    if (feedback != nullptr) {
      removePresentationFeedback(*feedback, false);
    }
  }

  void cancelAllPresentationFeedback() {
    for (PresentationFeedback& feedback : presentationFeedbacks) {
      if (feedback.proxy != nullptr) {
        wp_presentation_feedback_destroy(feedback.proxy);
      }
    }
    presentationFeedbacks.clear();
    NOCTALIA_TRACE_PLOT("Wayland presentation feedback pending", static_cast<std::int64_t>(0));
  }

  void createSurface() {
    const VkWaylandSurfaceCreateInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display = graphics.waylandDisplay(),
        .surface = waylandSurface,
    };
    requireVulkan(vkCreateWaylandSurfaceKHR(graphics.instance(), &info, nullptr, &surface), "vkCreateWaylandSurfaceKHR");
  }

  void recreateSurface() {
    cancelAllPresentationFeedback();
    if (generation != nullptr) {
      destroyGeneration(*generation, !deviceLost);
      generation.reset();
    }
    for (auto& old : retired) {
      destroyGeneration(*old, !deviceLost);
    }
    retired.clear();
    for (auto& frame : frames) {
      frame.acquirePending = false;
      frame.lastImage.reset();
    }
    if (surface != VK_NULL_HANDLE) {
      vkDestroySurfaceKHR(graphics.instance(), surface, nullptr);
      surface = VK_NULL_HANDLE;
    }
    createSurface();
    swapchainReason = SwapchainReason::Initial;
    swapchainInvalid = true;
    surfaceInvalid = false;
    createSwapchain();
  }

  bool recoverLostSurface() noexcept {
    try {
      recreateSurface();
      kLog.info("recreated Vulkan Wayland surface after VK_ERROR_SURFACE_LOST_KHR");
      return true;
    } catch (const std::exception& error) {
      surfaceInvalid = true;
      kLog.error("failed to recreate lost Vulkan Wayland surface: {}", error.what());
      return false;
    }
  }

  void createFrameSlots() {
    const VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (auto& frame : frames) {
      requireVulkan(
          vkCreateFence(graphics.device(), &fenceInfo, nullptr, &frame.acquireFence),
          "vkCreateFence(acquire)"
      );
    }
  }

  void awaitPresentation(const SwapchainImage& image) {
    if (!image.presentationPending) {
      return;
    }
    NOCTALIA_TRACE_ZONE("Graphite presentation-fence wait");
    requireVulkan(
        vkWaitForFences(graphics.device(), 1, &image.presentationFence, VK_TRUE, UINT64_MAX),
        "vkWaitForFences(presentation)"
    );
  }

  void waitPresentation(SwapchainImage& image) {
    awaitPresentation(image);
    if (!image.presentationPending) {
      return;
    }
    requireVulkan(vkResetFences(graphics.device(), 1, &image.presentationFence), "vkResetFences(presentation)");
    image.presentationPending = false;
  }

  bool generationComplete(const Generation& candidate) const {
    for (const auto& image : candidate.images) {
      if (image.presentationPending
          && vkGetFenceStatus(graphics.device(), image.presentationFence) != VK_SUCCESS) {
        return false;
      }
    }
    return true;
  }

  void destroyGeneration(Generation& candidate, bool wait) {
    for (auto& image : candidate.images) {
      if (wait) {
        // The generation is going away, so leave a signaled presentation
        // fence signaled. Resetting it is only necessary when the same image
        // and fence will be submitted again.
        awaitPresentation(image);
        image.presentationPending = false;
      }
      image.surface.reset();
      image.backendTexture = {};
      if (image.presentSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(graphics.device(), image.presentSemaphore, nullptr);
      }
      if (image.presentationFence != VK_NULL_HANDLE) {
        vkDestroyFence(graphics.device(), image.presentationFence, nullptr);
      }
    }
    candidate.images.clear();
    if (candidate.swapchain != VK_NULL_HANDLE) {
      vkDestroySwapchainKHR(graphics.device(), candidate.swapchain, nullptr);
      candidate.swapchain = VK_NULL_HANDLE;
    }
  }

  void collectRetired() {
    for (auto it = retired.begin(); it != retired.end();) {
      if (!generationComplete(**it)) {
        ++it;
        continue;
      }
      destroyGeneration(**it, true);
      it = retired.erase(it);
    }
  }

  void createSwapchain() {
    NOCTALIA_TRACE_ZONE("Graphite create swapchain");
    if (requestedWidth == 0 || requestedHeight == 0) {
      return;
    }
    VkBool32 presentSupport = VK_FALSE;
    requireVulkan(
        vkGetPhysicalDeviceSurfaceSupportKHR(
            graphics.physicalDevice(), graphics.graphicsQueueFamily(), surface, &presentSupport
        ),
        "vkGetPhysicalDeviceSurfaceSupportKHR"
    );
    if (presentSupport == VK_FALSE) {
      throw std::runtime_error("selected graphics queue cannot present to this Wayland surface");
    }

    VkSurfaceCapabilitiesKHR capabilities{};
    requireVulkan(
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(graphics.physicalDevice(), surface, &capabilities),
        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"
    );
    if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) == 0) {
      throw std::runtime_error("Wayland Vulkan surface does not support premultiplied composite alpha");
    }
    if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
      throw std::runtime_error("Wayland Vulkan surface images cannot be color attachments");
    }
    if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) == 0) {
      throw std::runtime_error("Wayland Vulkan surface images cannot be Graphite input attachments");
    }

    std::uint32_t formatCount = 0;
    requireVulkan(
        vkGetPhysicalDeviceSurfaceFormatsKHR(graphics.physicalDevice(), surface, &formatCount, nullptr),
        "vkGetPhysicalDeviceSurfaceFormatsKHR"
    );
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    requireVulkan(
        vkGetPhysicalDeviceSurfaceFormatsKHR(graphics.physicalDevice(), surface, &formatCount, formats.data()),
        "vkGetPhysicalDeviceSurfaceFormatsKHR"
    );
    formats.resize(formatCount);
    const auto chosenFormat = chooseSurfaceFormat(formats);
    const auto chosenExtent = chooseExtent(capabilities, requestedWidth, requestedHeight);
    if (chosenExtent.width == 0 || chosenExtent.height == 0) {
      return;
    }
    const std::uint32_t imageCount = std::clamp(
        capabilities.minImageCount + 1, capabilities.minImageCount,
        capabilities.maxImageCount == 0 ? std::numeric_limits<std::uint32_t>::max() : capabilities.maxImageCount
    );
    // Graphite requires input-attachment capability for every color render
    // target so it can implement destination reads without a separate copy.
    const VkImageUsageFlags imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
        | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT
        | (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    const VkSwapchainCreateInfoKHR createInfo{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = imageCount,
        .imageFormat = chosenFormat.format,
        .imageColorSpace = chosenFormat.colorSpace,
        .imageExtent = chosenExtent,
        .imageArrayLayers = 1,
        .imageUsage = imageUsage,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = generation != nullptr ? generation->swapchain : VK_NULL_HANDLE,
    };
    auto next = std::make_unique<Generation>();
    requireVulkan(vkCreateSwapchainKHR(graphics.device(), &createInfo, nullptr, &next->swapchain), "vkCreateSwapchainKHR");
    next->format = chosenFormat.format;
    next->extent = chosenExtent;

    std::uint32_t actualImageCount = 0;
    requireVulkan(
        vkGetSwapchainImagesKHR(graphics.device(), next->swapchain, &actualImageCount, nullptr),
        "vkGetSwapchainImagesKHR"
    );
    std::vector<VkImage> images(actualImageCount);
    requireVulkan(
        vkGetSwapchainImagesKHR(graphics.device(), next->swapchain, &actualImageCount, images.data()),
        "vkGetSwapchainImagesKHR"
    );
    images.resize(actualImageCount);
    next->images.resize(actualImageCount);
    const VkSemaphoreCreateInfo semaphoreInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    const VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    const skgpu::graphite::VulkanTextureInfo textureInfo(
        VK_SAMPLE_COUNT_1_BIT, skgpu::Mipmapped::kNo, 0, chosenFormat.format, VK_IMAGE_TILING_OPTIMAL, imageUsage,
        VK_SHARING_MODE_EXCLUSIVE, VK_IMAGE_ASPECT_COLOR_BIT, skgpu::VulkanYcbcrConversionInfo{}
    );
    try {
      for (std::uint32_t index = 0; index < actualImageCount; ++index) {
        auto& image = next->images[index];
        image.image = images[index];
        requireVulkan(
            vkCreateSemaphore(graphics.device(), &semaphoreInfo, nullptr, &image.presentSemaphore),
            "vkCreateSemaphore(present)"
        );
        requireVulkan(
            vkCreateFence(graphics.device(), &fenceInfo, nullptr, &image.presentationFence),
            "vkCreateFence(presentation)"
        );
        image.backendTexture = skgpu::graphite::BackendTextures::MakeVulkan(
            SkISize::Make(static_cast<int>(chosenExtent.width), static_cast<int>(chosenExtent.height)), textureInfo,
            VK_IMAGE_LAYOUT_UNDEFINED, graphics.graphicsQueueFamily(), image.image, {}
        );
        if (!image.backendTexture.isValid()) {
          throw std::runtime_error("Skia rejected a Vulkan swapchain image");
        }
        image.surface = SkSurfaces::WrapBackendTexture(
            graphics.recorder(), image.backendTexture, SkColorSpace::MakeSRGB(), nullptr, nullptr, nullptr,
            "Noctalia Wayland swapchain"
        );
        if (image.surface == nullptr) {
          throw std::runtime_error("Skia failed to wrap a Vulkan swapchain image as a Graphite surface");
        }
      }
    } catch (...) {
      // The generation is not published until every Skia wrapper and sync
      // object exists, so make partial initialization transactional too.
      destroyGeneration(*next, false);
      throw;
    }

    if (generation != nullptr) {
      // Acquire semaphores belong to the frame slots rather than a swapchain
      // generation. A presentation fence proves the submission which waited
      // on each slot's semaphore has completed before the slot is reused.
      for (auto& frame : frames) {
        if (frame.acquirePending) {
          requireVulkan(
              vkWaitForFences(graphics.device(), 1, &frame.acquireFence, VK_TRUE, UINT64_MAX),
              "vkWaitForFences(retired acquire)"
          );
          requireVulkan(vkResetFences(graphics.device(), 1, &frame.acquireFence), "vkResetFences(acquire)");
          frame.acquirePending = false;
        }
        if (frame.lastImage.has_value() && *frame.lastImage < generation->images.size()) {
          awaitPresentation(generation->images[*frame.lastImage]);
        }
        frame.lastImage.reset();
      }
      retired.push_back(std::move(generation));
    }
    generation = std::move(next);
    swapchainInvalid = false;
    collectRetired();
    ++swapchainsCreated;
    NOCTALIA_TRACE_PLOT("Graphite swapchain reason", static_cast<std::int64_t>(swapchainReason));
    NOCTALIA_TRACE_PLOT(
        "Graphite swapchains created", static_cast<std::int64_t>(swapchainsCreated)
    );
    NOCTALIA_TRACE_PLOT(
        "Graphite retired swapchains", static_cast<std::int64_t>(retired.size())
    );
    kLog.info(
        "created FIFO swapchain {}x{} with {} images ({}) reason={}", chosenExtent.width, chosenExtent.height,
        actualImageCount, surfaceFormatName(chosenFormat.format),
        static_cast<std::int64_t>(swapchainReason)
    );
  }

  SkCanvas* begin(RenderFrameStatus& status) {
    NOCTALIA_TRACE_ZONE("Graphite WSI begin frame");
    status = RenderFrameStatus::Deferred;
    // Completion callbacks retire textures whose handles were invalidated in
    // earlier frames. Poll before recording new work so their backing images
    // are released promptly without a CPU wait.
    if (graphics.graphiteContext() != nullptr) {
      graphics.graphiteContext()->checkAsyncWorkCompletion();
    }
    if (surfaceInvalid) {
      if (!recoverLostSurface()) {
        status = RenderFrameStatus::SurfaceLost;
        return nullptr;
      }
    }
    if (generation == nullptr || generation->images.empty() || currentFrame != nullptr) {
      return nullptr;
    }
    if (swapchainInvalid) {
      status = RenderFrameStatus::RecreateSwapchain;
      return nullptr;
    }
    collectRetired();
    auto& frame = frames[nextFrame];
    if (frame.lastImage.has_value() && *frame.lastImage < generation->images.size()) {
      waitPresentation(generation->images[*frame.lastImage]);
    }
    frame.lastImage.reset();

    std::uint32_t imageIndex = frame.acquiredImage;
    if (!frame.acquirePending) {
      VkResult acquire = VK_SUCCESS;
      const auto injected = takeInjectedVulkanWsiFault(VulkanWsiCall::Acquire);
      if (injected.has_value()) {
        kLog.warn("injecting acquire result {}", vulkanResultName(injected->result));
      }
      if (injected.has_value() && injected->skipDriverCall) {
        acquire = injected->result;
      } else {
        NOCTALIA_TRACE_ZONE("Graphite vkAcquireNextImageKHR");
        acquire = vkAcquireNextImageKHR(
            graphics.device(), generation->swapchain, UINT64_MAX, VK_NULL_HANDLE,
            frame.acquireFence, &imageIndex
        );
        if (injected.has_value() && acquire == VK_SUCCESS) {
          acquire = injected->result;
        }
      }
      if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchainInvalid = true;
        swapchainReason = SwapchainReason::AcquireOutOfDate;
        status = RenderFrameStatus::RecreateSwapchain;
        return nullptr;
      }
      if (acquire == VK_SUBOPTIMAL_KHR) {
        // SUBOPTIMAL still returns a usable, acquired image and signals the
        // fence. Present it once, then rebuild on the following frame. This
        // avoids abandoning an acquired image and reusing a signaled fence.
        swapchainInvalid = true;
        swapchainReason = SwapchainReason::AcquireSuboptimal;
        status = RenderFrameStatus::Presented;
      } else {
        status = classifyVulkanWsiResult(acquire);
      }
      if (status == RenderFrameStatus::SurfaceLost) {
        surfaceInvalid = true;
        if (recoverLostSurface()) {
          status = RenderFrameStatus::RecreateSwapchain;
        }
      }
      if (status != RenderFrameStatus::Presented) {
        return nullptr;
      }
      frame.acquiredImage = imageIndex;
      frame.acquirePending = true;
    }
    const VkResult acquireReady = vkGetFenceStatus(graphics.device(), frame.acquireFence);
    if (acquireReady == VK_NOT_READY) {
      status = RenderFrameStatus::Deferred;
      return nullptr;
    }
    requireVulkan(acquireReady, "vkGetFenceStatus(acquire)");
    requireVulkan(vkResetFences(graphics.device(), 1, &frame.acquireFence), "vkResetFences(acquire)");
    frame.acquirePending = false;
    imageIndex = frame.acquiredImage;
    auto& image = generation->images[imageIndex];
    waitPresentation(image);
    currentFrame = &frame;
    currentImage = &image;
    currentImageIndex = imageIndex;
    status = RenderFrameStatus::Presented;
    return image.surface->getCanvas();
  }

  RenderFrameStatus end(const std::function<void()>& recordingSubmitted) {
    NOCTALIA_TRACE_ZONE("Graphite WSI end frame");
    if (currentFrame == nullptr || currentImage == nullptr || generation == nullptr) {
      return RenderFrameStatus::Failed;
    }
    std::unique_ptr<skgpu::graphite::Recording> recording;
    {
      NOCTALIA_TRACE_ZONE("Graphite recorder snap");
      recording = graphics.recorder()->snap();
    }
    if (recording == nullptr) {
      currentFrame = nullptr;
      currentImage = nullptr;
      return RenderFrameStatus::Failed;
    }
    auto signalSemaphore = skgpu::graphite::BackendSemaphores::MakeVulkan(currentImage->presentSemaphore);
    auto presentState = skgpu::MutableTextureStates::MakeVulkan(
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, graphics.graphicsQueueFamily()
    );
    skgpu::graphite::InsertRecordingInfo insertInfo;
    insertInfo.fRecording = recording.get();
    insertInfo.fNumSignalSemaphores = 1;
    insertInfo.fSignalSemaphores = &signalSemaphore;
    insertInfo.fTargetSurface = currentImage->surface.get();
    insertInfo.fTargetTextureState = &presentState;
    {
      NOCTALIA_TRACE_ZONE("Graphite insert recording");
      if (!graphics.graphiteContext()->insertRecording(insertInfo)) {
        currentFrame = nullptr;
        currentImage = nullptr;
        return RenderFrameStatus::Failed;
      }
    }
    const auto submitFault = takeInjectedVulkanWsiFault(VulkanWsiCall::GraphiteSubmit);
    if (submitFault.has_value()) {
      kLog.warn("injecting Graphite submit result {}", vulkanResultName(submitFault->result));
    }
    {
      NOCTALIA_TRACE_ZONE("Graphite submit");
      const bool submitted = graphics.graphiteContext()->submit(
          submitFault.has_value() ? skgpu::graphite::SyncToCpu::kYes : skgpu::graphite::SyncToCpu::kNo
      );
      if (!submitted) {
        currentFrame = nullptr;
        currentImage = nullptr;
        return RenderFrameStatus::DeviceLost;
      }
    }
    if (recordingSubmitted) {
      recordingSubmitted();
    }
    if (submitFault.has_value()) {
      // The test recording really completed, so its resources can be torn down
      // normally. Report device loss only after honoring the submission and
      // external-image completion contracts.
      currentFrame = nullptr;
      currentImage = nullptr;
      injectedDeviceLoss = true;
      return RenderFrameStatus::DeviceLost;
    }

    const VkSwapchainPresentFenceInfoKHR fenceInfo{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
        .swapchainCount = 1,
        .pFences = &currentImage->presentationFence,
    };
    const VkPresentInfoKHR presentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = &fenceInfo,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &currentImage->presentSemaphore,
        .swapchainCount = 1,
        .pSwapchains = &generation->swapchain,
        .pImageIndices = &currentImageIndex,
    };
    // Vulkan's Wayland WSI performs the wl_surface commit inside present.
    // Asking immediately beforehand binds this feedback object to that exact
    // commit, rather than to a later frame submitted by another code path.
    PresentationFeedback* presentationFeedback = requestPresentationFeedback();
    VkResult present = VK_SUCCESS;
    const auto injected = takeInjectedVulkanWsiFault(VulkanWsiCall::Present);
    if (injected.has_value()) {
      kLog.warn("injecting present result {}", vulkanResultName(injected->result));
    }
    {
      NOCTALIA_TRACE_ZONE("Graphite vkQueuePresentKHR");
      present = vkQueuePresentKHR(graphics.graphicsQueue(), &presentInfo);
    }
    if (injected.has_value() && present == VK_SUCCESS) {
      present = injected->result;
    }
    const tracy_latency::PresentationTrace presentationTrace = tracy_latency::presentationSubmitted();
    if (presentationFeedback != nullptr) {
      presentationFeedback->trace = presentationTrace;
      if (present != VK_SUCCESS && present != VK_SUBOPTIMAL_KHR) {
        cancelPresentationFeedback(presentationFeedback);
      }
    }
    // A present fence covers the presentation request even when the WSI call
    // reports OUT_OF_DATE or SURFACE_LOST. Those failure results do not mean
    // the queue has stopped referencing the semaphore/fence. DEVICE_LOST is
    // the sole result for which waiting on further device work is invalid.
    currentImage->presentationPending = present != VK_ERROR_DEVICE_LOST;
    currentFrame->lastImage = currentImageIndex;
    currentFrame = nullptr;
    currentImage = nullptr;
    nextFrame = (nextFrame + 1) % kFramesInFlight;
    const RenderFrameStatus status = classifyVulkanWsiResult(present);
    if (present == VK_ERROR_OUT_OF_DATE_KHR) {
      swapchainInvalid = true;
      swapchainReason = SwapchainReason::PresentOutOfDate;
    } else if (present == VK_SUBOPTIMAL_KHR) {
      swapchainInvalid = true;
      swapchainReason = SwapchainReason::PresentSuboptimal;
    } else if (present == VK_ERROR_SURFACE_LOST_KHR) {
      surfaceInvalid = true;
      (void)recoverLostSurface();
    }
    NOCTALIA_TRACE_PLOT("Graphite present result", static_cast<std::int64_t>(present));
    NOCTALIA_TRACE_FRAME("Graphite presented frame");
    return status;
  }

  void cleanup() {
    currentFrame = nullptr;
    currentImage = nullptr;
    cancelAllPresentationFeedback();
    if (graphics.device() == VK_NULL_HANDLE) {
      generation.reset();
      retired.clear();
      surface = VK_NULL_HANDLE;
      return;
    }
    if (generation != nullptr) {
      destroyGeneration(*generation, !deviceLost);
      generation.reset();
    }
    for (auto& old : retired) {
      destroyGeneration(*old, !deviceLost);
    }
    retired.clear();
    for (auto& frame : frames) {
      if (frame.acquirePending && !deviceLost) {
        (void)vkWaitForFences(graphics.device(), 1, &frame.acquireFence, VK_TRUE, UINT64_MAX);
      }
      frame.acquirePending = false;
      if (frame.acquireFence != VK_NULL_HANDLE) {
        vkDestroyFence(graphics.device(), frame.acquireFence, nullptr);
        frame.acquireFence = VK_NULL_HANDLE;
      }
      frame.lastImage.reset();
    }
    if (surface != VK_NULL_HANDLE) {
      vkDestroySurfaceKHR(graphics.instance(), surface, nullptr);
      surface = VK_NULL_HANDLE;
    }
  }
};

GraphiteSurfaceTarget::GraphiteSurfaceTarget(GraphicsDevice& graphics, wl_surface* surface)
    : m_impl(std::make_unique<Impl>(graphics, surface)) {
  if (!graphics.valid() || surface == nullptr) {
    throw std::runtime_error("GraphiteSurfaceTarget requires a valid GraphicsDevice and wl_surface");
  }
  try {
    m_impl->createSurface();
    m_impl->createFrameSlots();
  } catch (...) {
    m_impl->cleanup();
    throw;
  }
}

GraphiteSurfaceTarget::~GraphiteSurfaceTarget() { destroy(); }

void GraphiteSurfaceTarget::resize(std::uint32_t width, std::uint32_t height) {
  if (m_impl == nullptr) {
    return;
  }
  m_impl->requestedWidth = width;
  m_impl->requestedHeight = height;
  if (width == 0 || height == 0) {
    return;
  }
  if (!m_impl->swapchainInvalid && m_impl->generation != nullptr && m_impl->generation->extent.width == width
      && m_impl->generation->extent.height == height) {
    return;
  }
  if (!m_impl->swapchainInvalid && m_impl->generation != nullptr) {
    m_impl->swapchainReason = Impl::SwapchainReason::Resize;
  }
  m_impl->createSwapchain();
}

void GraphiteSurfaceTarget::setPresentationCallback(SurfacePresentationCallback callback) {
  if (m_impl != nullptr) {
    m_impl->presentationCallback = std::move(callback);
  }
}

void GraphiteSurfaceTarget::abandonDevice() noexcept {
  if (m_impl == nullptr) {
    return;
  }
  m_impl->deviceLost = true;
  m_impl->currentFrame = nullptr;
  m_impl->currentImage = nullptr;
  for (auto& frame : m_impl->frames) {
    frame.acquirePending = false;
    frame.lastImage.reset();
  }
  if (m_impl->generation != nullptr) {
    for (auto& image : m_impl->generation->images) {
      image.presentationPending = false;
    }
  }
  for (auto& retired : m_impl->retired) {
    for (auto& image : retired->images) {
      image.presentationPending = false;
    }
  }
}

void GraphiteSurfaceTarget::destroy() {
  if (m_impl != nullptr) {
    m_impl->cleanup();
    m_impl.reset();
  }
}

bool GraphiteSurfaceTarget::ready() const noexcept {
  return m_impl != nullptr && m_impl->generation != nullptr && !m_impl->generation->images.empty();
}

SkCanvas* GraphiteSurfaceTarget::beginFrame(RenderFrameStatus& status) {
  return m_impl != nullptr ? m_impl->begin(status) : nullptr;
}

RenderFrameStatus GraphiteSurfaceTarget::endFrame(const std::function<void()>& recordingSubmitted) {
  return m_impl != nullptr ? m_impl->end(recordingSubmitted) : RenderFrameStatus::Failed;
}

bool GraphiteSurfaceTarget::deviceLossWasInjected() const noexcept {
  return m_impl != nullptr && m_impl->injectedDeviceLoss;
}

VkFormat GraphiteSurfaceTarget::format() const noexcept {
  return m_impl != nullptr && m_impl->generation != nullptr ? m_impl->generation->format : VK_FORMAT_UNDEFINED;
}

VkExtent2D GraphiteSurfaceTarget::extent() const noexcept {
  return m_impl != nullptr && m_impl->generation != nullptr ? m_impl->generation->extent : VkExtent2D{};
}
