#include "cef/cef_gpu_frame_bridge.h"

#include "core/log.h"
#include "core/tracy.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSize.h"
#include "include/gpu/GpuTypes.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/Image.h"
#include "include/gpu/graphite/vk/VulkanGraphiteTypes.h"
#include "render/backend/graphite_texture_manager.h"
#include "render/graphics_device.h"
#include "render/vulkan/vulkan_result.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
  constexpr Logger kLog("cef-vulkan");
  constexpr VkExternalMemoryHandleTypeFlagBits kDmabufHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  constexpr VkExternalSemaphoreHandleTypeFlagBits kSyncFdHandle = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
  constexpr std::size_t kMaxCachedImports = 5;
  constexpr std::size_t kFrameSlots = 3;
  // Match Chromium's VulkanImage::CreateFromGpuMemoryBufferHandle contract.
  // DRM modifiers describe the storage layout, but drivers are allowed to
  // select usage-dependent metadata and memory requirements for the aliasing
  // VkImage. Recreating the imported image with the producer's complete usage
  // mask avoids describing the same DMA-BUF through an incompatible image
  // contract on the consumer device.
  constexpr VkImageUsageFlags kCefImportedImageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
      | VK_IMAGE_USAGE_SAMPLED_BIT
      | VK_IMAGE_USAGE_TRANSFER_DST_BIT
      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

  constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(a)
        | (static_cast<std::uint32_t>(b) << 8U)
        | (static_cast<std::uint32_t>(c) << 16U)
        | (static_cast<std::uint32_t>(d) << 24U);
  }
  constexpr std::uint32_t kDrmArgb8888 = fourcc('A', 'R', '2', '4');
  constexpr std::uint32_t kDrmAbgr8888 = fourcc('A', 'B', '2', '4');
  constexpr std::uint64_t kDrmModifierInvalid = (UINT64_C(1) << 56U) - 1U;

  VkFormat formatForFourcc(std::uint32_t value) {
    switch (value) {
    case kDrmArgb8888:
      return VK_FORMAT_B8G8R8A8_UNORM;
    case kDrmAbgr8888:
      return VK_FORMAT_R8G8B8A8_UNORM;
    default:
      return VK_FORMAT_UNDEFINED;
    }
  }

  std::uint32_t
  chooseMemoryType(VkPhysicalDevice physicalDevice, std::uint32_t typeBits, VkMemoryPropertyFlags preferred) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
    for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
      if ((typeBits & (1U << index)) != 0 && (properties.memoryTypes[index].propertyFlags & preferred) == preferred) {
        return index;
      }
    }
    for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
      if ((typeBits & (1U << index)) != 0) {
        return index;
      }
    }
    throw std::runtime_error("no compatible Vulkan memory type");
  }

  struct ImportedSource {
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    int duplicatedFd = -1;
    CefGpuFrameBridgeStats* stats = nullptr;
    bool tracked = false;

    ~ImportedSource() {
      if (image != VK_NULL_HANDLE) {
        vkDestroyImage(device, image, nullptr);
      }
      if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, memory, nullptr);
      } else if (duplicatedFd >= 0) {
        ::close(duplicatedFd);
      }
      if (tracked && stats != nullptr) {
        ++stats->importsDestroyed;
        --stats->activeImports;
      }
    }
  };

  struct ImportKey {
    std::uint64_t transportEpoch = 0;
    std::uint64_t fileDevice = 0;
    std::uint64_t inode = 0;
    int width = 0;
    int height = 0;
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
    std::uint32_t stride = 0;
    std::uint64_t offset = 0;

    bool operator==(const ImportKey&) const = default;
  };

  struct CachedImport {
    ImportKey key;
    // source owns the Vulkan image and memory. Keep it declared before the
    // Graphite wrappers so reverse member destruction releases SkImage first.
    std::unique_ptr<ImportedSource> source;
    skgpu::graphite::BackendTexture backendTexture;
    sk_sp<SkImage> skImage;
    std::uint64_t lastUsed = 0;
    std::uint64_t lastCompletionValue = 0;
  };

} // namespace

struct CefGpuFrameBridge::Impl {
  struct PendingRelease {
    std::uint64_t transportEpoch = 0;
    std::int64_t captureCounter = -1;
    int fd = -1;
  };

  struct FrameSlot {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkCommandBuffer releaseCommandBuffer = VK_NULL_HANDLE;
    VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
    VkSemaphore acquireCompleteSemaphore = VK_NULL_HANDLE;
    VkSemaphore samplingCompleteSemaphore = VK_NULL_HANDLE;
    VkSemaphore releaseSemaphore = VK_NULL_HANDLE;
    std::uint64_t completionValue = 0;
  };

  struct PendingFrame {
    FrameSlot* slot = nullptr;
    CachedImport* imported = nullptr;
    std::uint64_t transportEpoch = 0;
    std::int64_t captureCounter = -1;
    std::uint64_t outputGeneration = 0;
    std::uint64_t contentSerial = 0;
    std::uint32_t queueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    VkImageLayout producerOldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout producerNewLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool prepared = false;
    bool acquireCompletionWait = false;
    bool sampled = false;
    std::uint64_t graphiteCompletionValue = 0;
  };

  GraphicsDevice& graphics;
  GraphiteTextureManager& textures;
  VkCommandPool commandPool = VK_NULL_HANDLE;
  std::array<FrameSlot, kFrameSlots> frameSlots{};
  std::size_t nextFrameSlot = 0;
  VkSemaphore completionTimeline = VK_NULL_HANDLE;
  std::uint64_t nextCompletionValue = 0;
  bool deviceAbandoned = false;
  ReleaseFenceCallback releaseFenceCallback;
  GraphiteExternalImageSynchronization* synchronizationOwner = nullptr;
  PFN_vkGetMemoryFdPropertiesKHR getMemoryFdProperties = nullptr;
  PFN_vkImportSemaphoreFdKHR importSemaphoreFd = nullptr;
  PFN_vkGetSemaphoreFdKHR getSemaphoreFd = nullptr;

  TextureHandle textureHandle;
  std::uint64_t importUseSequence = 0;
  std::vector<CachedImport> importCache;
  std::optional<PendingFrame> pendingFrame;
  std::uint64_t lastOutputGeneration = 0;
  std::uint64_t lastContentSerial = 0;
  std::uint64_t lastTransportEpoch = 0;
  CefGpuFrameBridgeStats lifetimeStats;
  std::string error;
  std::mutex mutex;

  explicit Impl(
      GraphicsDevice& device, GraphiteTextureManager& manager, ReleaseFenceCallback callback,
      GraphiteExternalImageSynchronization* owner
  )
      : graphics(device), textures(manager), releaseFenceCallback(std::move(callback)), synchronizationOwner(owner) {
    if (!graphics.valid() || !graphics.cefExternalMemoryEnabled()) {
      throw std::runtime_error("CEF GPU bridge requires a Vulkan device with external-memory support");
    }
    if (!releaseFenceCallback) {
      throw std::runtime_error("CEF GPU bridge requires a release-fence callback");
    }
    kLog.info("CEF bridge enabled direct Graphite DMA-BUF sampling");
    getMemoryFdProperties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
        vkGetDeviceProcAddr(graphics.device(), "vkGetMemoryFdPropertiesKHR")
    );
    if (getMemoryFdProperties == nullptr) {
      throw std::runtime_error("Vulkan loader did not expose vkGetMemoryFdPropertiesKHR");
    }
    importSemaphoreFd =
        reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(graphics.device(), "vkImportSemaphoreFdKHR"));
    if (importSemaphoreFd == nullptr) {
      throw std::runtime_error("Vulkan loader did not expose vkImportSemaphoreFdKHR");
    }
    getSemaphoreFd =
        reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(graphics.device(), "vkGetSemaphoreFdKHR"));
    if (getSemaphoreFd == nullptr) {
      throw std::runtime_error("Vulkan loader did not expose vkGetSemaphoreFdKHR");
    }
    const VkCommandPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = graphics.graphicsQueueFamily(),
    };
    requireVulkan(vkCreateCommandPool(graphics.device(), &poolInfo, nullptr, &commandPool), "vkCreateCommandPool(CEF)");
    const VkCommandBufferAllocateInfo allocateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = static_cast<std::uint32_t>(frameSlots.size() * 2U),
    };
    std::array<VkCommandBuffer, kFrameSlots * 2U> commandBuffers{};
    requireVulkan(
        vkAllocateCommandBuffers(graphics.device(), &allocateInfo, commandBuffers.data()),
        "vkAllocateCommandBuffers(CEF)"
    );
    const VkSemaphoreTypeCreateInfo timelineTypeInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    const VkSemaphoreCreateInfo timelineInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timelineTypeInfo,
    };
    requireVulkan(
        vkCreateSemaphore(graphics.device(), &timelineInfo, nullptr, &completionTimeline),
        "vkCreateSemaphore(CEF completion timeline)"
    );
    const VkSemaphoreCreateInfo semaphoreInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    };
    const VkExportSemaphoreCreateInfo exportInfo{
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = kSyncFdHandle,
    };
    const VkSemaphoreCreateInfo releaseSemaphoreInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &exportInfo,
    };
    for (std::size_t index = 0; index < frameSlots.size(); ++index) {
      frameSlots[index].commandBuffer = commandBuffers[index];
      frameSlots[index].releaseCommandBuffer = commandBuffers[index + frameSlots.size()];
      requireVulkan(
          vkCreateSemaphore(graphics.device(), &semaphoreInfo, nullptr, &frameSlots[index].acquireSemaphore),
          "vkCreateSemaphore(CEF acquire ring)"
      );
      requireVulkan(
          vkCreateSemaphore(graphics.device(), &semaphoreInfo, nullptr, &frameSlots[index].acquireCompleteSemaphore),
          "vkCreateSemaphore(CEF acquire completion ring)"
      );
      requireVulkan(
          vkCreateSemaphore(graphics.device(), &semaphoreInfo, nullptr, &frameSlots[index].samplingCompleteSemaphore),
          "vkCreateSemaphore(CEF sampling completion ring)"
      );
      requireVulkan(
          vkCreateSemaphore(graphics.device(), &releaseSemaphoreInfo, nullptr, &frameSlots[index].releaseSemaphore),
          "vkCreateSemaphore(CEF release ring)"
      );
    }
  }

  ~Impl() { cleanup(); }

  void setError(std::string message) {
    error = std::move(message);
    kLog.error("{}", error);
  }

  void waitForGraphiteUse() {
    NOCTALIA_TRACE_ZONE("CEF wait for Graphite texture use");
    if (graphics.graphiteContext() != nullptr) {
      // Resizes and invalidation are exceptional paths. Waiting here keeps an
      // old externally-owned VkImage alive until all Graphite sampling ends.
      (void)graphics.graphiteContext()->submit(skgpu::graphite::SyncToCpu::kYes);
    }
  }

  void waitForCompletion(std::uint64_t value) {
    if (value == 0 || completionTimeline == VK_NULL_HANDLE) {
      return;
    }
    const VkSemaphoreWaitInfo waitInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &completionTimeline,
        .pValues = &value,
    };
    requireVulkan(
        vkWaitSemaphores(graphics.device(), &waitInfo, UINT64_MAX), "vkWaitSemaphores(CEF completion timeline)"
    );
  }

  FrameSlot& acquireFrameSlot() {
    FrameSlot& slot = frameSlots[nextFrameSlot];
    nextFrameSlot = (nextFrameSlot + 1) % frameSlots.size();
    waitForCompletion(slot.completionValue);
    return slot;
  }

  void releaseTexture() {
    if (textureHandle.valid()) {
      textures.unload(textureHandle);
    }
    textureHandle = {};
  }

  void cleanup() {
    if (graphics.device() == VK_NULL_HANDLE) {
      return;
    }
    if (!deviceAbandoned) {
      try {
        deliverRelease(releasePendingDirectFrame());
      } catch (const std::exception& exception) {
        kLog.error("failed to release pending direct CEF frame during cleanup: {}", exception.what());
      }
      (void)vkDeviceWaitIdle(graphics.device());
    } else {
      pendingFrame.reset();
    }
    releaseTexture();
    importCache.clear();
    for (FrameSlot& slot : frameSlots) {
      if (slot.acquireSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(graphics.device(), slot.acquireSemaphore, nullptr);
        slot.acquireSemaphore = VK_NULL_HANDLE;
      }
      if (slot.acquireCompleteSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(graphics.device(), slot.acquireCompleteSemaphore, nullptr);
        slot.acquireCompleteSemaphore = VK_NULL_HANDLE;
      }
      if (slot.samplingCompleteSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(graphics.device(), slot.samplingCompleteSemaphore, nullptr);
        slot.samplingCompleteSemaphore = VK_NULL_HANDLE;
      }
      if (slot.releaseSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(graphics.device(), slot.releaseSemaphore, nullptr);
        slot.releaseSemaphore = VK_NULL_HANDLE;
      }
      slot.commandBuffer = VK_NULL_HANDLE;
      slot.releaseCommandBuffer = VK_NULL_HANDLE;
      slot.completionValue = 0;
    }
    if (completionTimeline != VK_NULL_HANDLE) {
      vkDestroySemaphore(graphics.device(), completionTimeline, nullptr);
      completionTimeline = VK_NULL_HANDLE;
    }
    if (commandPool != VK_NULL_HANDLE) {
      vkDestroyCommandPool(graphics.device(), commandPool, nullptr);
      commandPool = VK_NULL_HANDLE;
    }
  }

  void deliverRelease(std::optional<PendingRelease> release) noexcept {
    if (!release || release->fd < 0) {
      return;
    }
    try {
      releaseFenceCallback(release->transportEpoch, release->captureCounter, release->fd);
    } catch (...) {
      // An embedder callback must never unwind through bridge teardown or a CEF
      // callback boundary.
    }
    close(release->fd);
  }

  bool modifierSupported(VkFormat candidateFormat, std::uint64_t modifier) const {
    VkDrmFormatModifierPropertiesListEXT list{
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 properties{
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &list,
    };
    vkGetPhysicalDeviceFormatProperties2(graphics.physicalDevice(), candidateFormat, &properties);
    std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = modifiers.data();
    vkGetPhysicalDeviceFormatProperties2(graphics.physicalDevice(), candidateFormat, &properties);
    return std::ranges::any_of(modifiers, [modifier](const auto& candidate) {
      return candidate.drmFormatModifier == modifier
          && candidate.drmFormatModifierPlaneCount == 1
          && (candidate.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    });
  }

  bool externalImportSupported(VkFormat candidateFormat, std::uint64_t modifier) const {
    VkPhysicalDeviceExternalImageFormatInfo externalInfo{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = kDmabufHandle,
    };
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierInfo{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .pNext = &externalInfo,
        .drmFormatModifier = modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkPhysicalDeviceImageFormatInfo2 input{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &modifierInfo,
        .format = candidateFormat,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = kCefImportedImageUsage,
    };
    VkExternalImageFormatProperties externalProperties{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 output{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &externalProperties,
    };
    const VkResult result = vkGetPhysicalDeviceImageFormatProperties2(graphics.physicalDevice(), &input, &output);
    if (result != VK_SUCCESS) {
      return false;
    }
    const auto features = externalProperties.externalMemoryProperties.externalMemoryFeatures;
    const auto compatible = externalProperties.externalMemoryProperties.compatibleHandleTypes;
    return (features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0 && (compatible & kDmabufHandle) != 0;
  }

  void validate(const BorrowedDmabufFrame& frame, VkFormat candidateFormat) const {
    NOCTALIA_TRACE_ZONE("CEF validate DMA-BUF");
    if (frame.width <= 0 || frame.height <= 0 || frame.width > 32768 || frame.height > 32768) {
      throw std::runtime_error("CEF DMA-BUF has invalid dimensions");
    }
    if (candidateFormat == VK_FORMAT_UNDEFINED) {
      throw std::runtime_error(std::format("unsupported CEF DRM fourcc 0x{:08x}", frame.fourcc));
    }
    if (frame.modifier == kDrmModifierInvalid) {
      throw std::runtime_error("CEF DMA-BUF did not provide a valid DRM modifier");
    }
    if (frame.planeCount != 1 || frame.planes[0].fd < 0) {
      throw std::runtime_error("CEF RGBA/BGRA DMA-BUF must contain exactly one valid plane");
    }
    if (frame.acquireFenceFd < 0) {
      throw std::runtime_error("CEF DMA-BUF did not provide a producer acquire fence");
    }
    if (frame.producerOldLayout == VK_IMAGE_LAYOUT_UNDEFINED
        || frame.producerOldLayout == VK_IMAGE_LAYOUT_PREINITIALIZED
        || frame.producerNewLayout == VK_IMAGE_LAYOUT_UNDEFINED
        || frame.producerNewLayout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
      throw std::runtime_error(
          std::format(
              "CEF DMA-BUF has invalid producer layout pair {} -> {}", frame.producerOldLayout, frame.producerNewLayout
          )
      );
    }
    if (frame.queueFamilyIndex != VK_QUEUE_FAMILY_EXTERNAL_KHR) {
      throw std::runtime_error(
          std::format("CEF DMA-BUF has unsupported handoff queue family {}", frame.queueFamilyIndex)
      );
    }
    if (frame.planes[0].stride < static_cast<std::uint32_t>(frame.width) * 4U) {
      throw std::runtime_error("CEF DMA-BUF stride is smaller than its pixel width");
    }
    if (!modifierSupported(candidateFormat, frame.modifier)) {
      throw std::runtime_error("CEF DMA-BUF modifier is not sampleable on the selected GPU");
    }
    if (!externalImportSupported(candidateFormat, frame.modifier)) {
      throw std::runtime_error("CEF DMA-BUF format/modifier is not importable as external Vulkan memory");
    }
  }

  std::unique_ptr<ImportedSource> importSource(const BorrowedDmabufFrame& frame, VkFormat candidateFormat) {
    NOCTALIA_TRACE_ZONE("CEF import DMA-BUF image");
    auto source = std::make_unique<ImportedSource>();
    source->device = graphics.device();
    source->stats = &lifetimeStats;
    source->tracked = true;
    ++lifetimeStats.importsCreated;
    ++lifetimeStats.activeImports;
    source->duplicatedFd = ::dup(frame.planes[0].fd);
    if (source->duplicatedFd < 0) {
      throw std::runtime_error(std::format("dup(CEF DMA-BUF) failed: {}", std::strerror(errno)));
    }

    const VkSubresourceLayout planeLayout{
        .offset = frame.planes[0].offset,
        .rowPitch = frame.planes[0].stride,
    };
    const VkExternalMemoryImageCreateInfo externalInfo{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = kDmabufHandle,
    };
    const VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .pNext = &externalInfo,
        .drmFormatModifier = frame.modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &planeLayout,
    };
    const VkImageCreateInfo imageInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &modifierInfo,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = candidateFormat,
        .extent = {static_cast<std::uint32_t>(frame.width), static_cast<std::uint32_t>(frame.height), 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = kCefImportedImageUsage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    requireVulkan(vkCreateImage(graphics.device(), &imageInfo, nullptr, &source->image), "vkCreateImage(CEF import)");

    VkMemoryDedicatedRequirements dedicatedRequirements{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 requirements{
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &dedicatedRequirements,
    };
    const VkImageMemoryRequirementsInfo2 requirementsInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = source->image,
    };
    vkGetImageMemoryRequirements2(graphics.device(), &requirementsInfo, &requirements);
    VkMemoryFdPropertiesKHR fdProperties{
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };
    requireVulkan(
        getMemoryFdProperties(graphics.device(), kDmabufHandle, source->duplicatedFd, &fdProperties),
        "vkGetMemoryFdPropertiesKHR(CEF import)"
    );
    const std::uint32_t compatibleMemoryTypes =
        requirements.memoryRequirements.memoryTypeBits & fdProperties.memoryTypeBits;
    if (compatibleMemoryTypes == 0) {
      throw std::runtime_error(
          std::format(
              "CEF DMA-BUF has no memory type compatible with the modifier image (image=0x{:08x}, fd=0x{:08x})",
              requirements.memoryRequirements.memoryTypeBits, fdProperties.memoryTypeBits
          )
      );
    }
    const std::uint32_t memoryTypeIndex = chooseMemoryType(graphics.physicalDevice(), compatibleMemoryTypes, 0);
    const VkImportMemoryFdInfoKHR importInfo{
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = kDmabufHandle,
        .fd = source->duplicatedFd,
    };
    const VkMemoryDedicatedAllocateInfo dedicatedInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &importInfo,
        .image = source->image,
    };
    const VkMemoryAllocateInfo allocationInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicatedInfo,
        .allocationSize = requirements.memoryRequirements.size,
        .memoryTypeIndex = memoryTypeIndex,
    };
    const VkResult allocation = vkAllocateMemory(graphics.device(), &allocationInfo, nullptr, &source->memory);
    if (allocation == VK_SUCCESS) {
      // Vulkan consumes the imported descriptor on successful allocation.
      source->duplicatedFd = -1;
    }
    requireVulkan(allocation, "vkAllocateMemory(CEF import)");
    requireVulkan(
        vkBindImageMemory(graphics.device(), source->image, source->memory, 0), "vkBindImageMemory(CEF import)"
    );
    return source;
  }

  ImportKey importKey(const BorrowedDmabufFrame& frame) const {
    struct stat fdStat{};
    if (fstat(frame.planes[0].fd, &fdStat) != 0) {
      throw std::runtime_error(std::format("fstat(CEF DMA-BUF) failed: {}", std::strerror(errno)));
    }
    return {
        .transportEpoch = frame.transportEpoch,
        .fileDevice = static_cast<std::uint64_t>(fdStat.st_dev),
        .inode = static_cast<std::uint64_t>(fdStat.st_ino),
        .width = frame.width,
        .height = frame.height,
        .fourcc = frame.fourcc,
        .modifier = frame.modifier,
        .stride = frame.planes[0].stride,
        .offset = frame.planes[0].offset,
    };
  }

  void wrapImportedSource(CachedImport& cached, VkFormat candidateFormat) {
    const skgpu::graphite::VulkanTextureInfo textureInfo(
        VK_SAMPLE_COUNT_1_BIT, skgpu::Mipmapped::kNo, 0, candidateFormat, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        kCefImportedImageUsage, VK_SHARING_MODE_EXCLUSIVE, VK_IMAGE_ASPECT_COLOR_BIT, skgpu::VulkanYcbcrConversionInfo{}
    );
    cached.backendTexture = skgpu::graphite::BackendTextures::MakeVulkan(
        SkISize::Make(cached.key.width, cached.key.height), textureInfo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        graphics.graphicsQueueFamily(), cached.source->image, {}
    );
    if (!cached.backendTexture.isValid()) {
      throw std::runtime_error("Skia rejected a cached CEF DMA-BUF import");
    }
    cached.skImage = SkImages::WrapTexture(
        graphics.recorder(), cached.backendTexture, kPremul_SkAlphaType, SkColorSpace::MakeSRGB()
    );
    if (cached.skImage == nullptr) {
      throw std::runtime_error("Skia failed to wrap a cached CEF DMA-BUF import");
    }
  }

  CachedImport& cachedSource(const BorrowedDmabufFrame& frame, VkFormat candidateFormat) {
    NOCTALIA_TRACE_ZONE("CEF find cached DMA-BUF import");
    const ImportKey key = importKey(frame);
    const auto found = std::ranges::find(importCache, key, &CachedImport::key);
    if (found != importCache.end()) {
      ++lifetimeStats.importCacheHits;
      found->lastUsed = ++importUseSequence;
      NOCTALIA_TRACE_PLOT("CEF DMA-BUF import cache hits", static_cast<std::int64_t>(lifetimeStats.importCacheHits));
      return *found;
    }

    ++lifetimeStats.importCacheMisses;
    if (importCache.size() >= kMaxCachedImports) {
      const auto oldest = std::ranges::min_element(importCache, {}, &CachedImport::lastUsed);
      waitForCompletion(oldest->lastCompletionValue);
      importCache.erase(oldest);
      ++lifetimeStats.importCacheEvictions;
      NOCTALIA_TRACE_PLOT(
          "CEF DMA-BUF import cache evictions", static_cast<std::int64_t>(lifetimeStats.importCacheEvictions)
      );
    }
    CachedImport cached{
        .key = key,
        .source = importSource(frame, candidateFormat),
        .lastUsed = ++importUseSequence,
    };
    wrapImportedSource(cached, candidateFormat);
    importCache.push_back(std::move(cached));
    NOCTALIA_TRACE_PLOT("CEF DMA-BUF import cache misses", static_cast<std::int64_t>(lifetimeStats.importCacheMisses));
    NOCTALIA_TRACE_PLOT("CEF DMA-BUF import cache entries", static_cast<std::int64_t>(importCache.size()));
    return importCache.back();
  }

  VkSemaphore importAcquireFence(const BorrowedDmabufFrame& frame, VkSemaphore semaphore) {
    NOCTALIA_TRACE_ZONE("CEF import acquire fence");
    int duplicatedFd = ::dup(frame.acquireFenceFd);
    if (duplicatedFd < 0) {
      throw std::runtime_error(std::format("dup(CEF acquire fence) failed: {}", std::strerror(errno)));
    }
    const VkImportSemaphoreFdInfoKHR importInfo{
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore = semaphore,
        .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
        .handleType = kSyncFdHandle,
        .fd = duplicatedFd,
    };
    const VkResult imported = importSemaphoreFd(graphics.device(), &importInfo);
    if (imported == VK_SUCCESS) {
      // A successful SYNC_FD import transfers descriptor ownership to Vulkan.
      duplicatedFd = -1;
    }
    if (duplicatedFd >= 0) {
      ::close(duplicatedFd);
    }
    requireVulkan(imported, "vkImportSemaphoreFdKHR(CEF acquire)");
    return semaphore;
  }

  void recordDirectAcquire(
      VkCommandBuffer commandBuffer, VkImage image, std::uint32_t handoffQueueFamily, VkImageLayout producerOldLayout,
      VkImageLayout producerNewLayout
  ) {
    NOCTALIA_TRACE_ZONE("CEF record direct-sampling acquire");
    requireVulkan(vkResetCommandBuffer(commandBuffer, 0), "vkResetCommandBuffer(CEF direct acquire)");
    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    requireVulkan(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer(CEF direct acquire)");
    const VkImageMemoryBarrier2 ownershipBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
        .oldLayout = producerOldLayout,
        .newLayout = producerNewLayout,
        .srcQueueFamilyIndex = handoffQueueFamily,
        .dstQueueFamilyIndex = graphics.graphicsQueueFamily(),
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    const VkDependencyInfo ownershipDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &ownershipBarrier,
    };
    vkCmdPipelineBarrier2(commandBuffer, &ownershipDependency);

    const VkImageMemoryBarrier2 layoutBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
        .oldLayout = producerNewLayout,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    const VkDependencyInfo layoutDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &layoutBarrier,
    };
    vkCmdPipelineBarrier2(commandBuffer, &layoutDependency);
    requireVulkan(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer(CEF direct acquire)");
  }

  void recordDirectRelease(VkCommandBuffer commandBuffer, VkImage image) {
    NOCTALIA_TRACE_ZONE("CEF record direct-sampling release");
    requireVulkan(vkResetCommandBuffer(commandBuffer, 0), "vkResetCommandBuffer(CEF direct release)");
    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    requireVulkan(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer(CEF direct release)");
    const VkImageMemoryBarrier2 layoutBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    const VkDependencyInfo layoutDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &layoutBarrier,
    };
    vkCmdPipelineBarrier2(commandBuffer, &layoutDependency);

    const VkImageMemoryBarrier2 ownershipBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
        .dstAccessMask = VK_ACCESS_2_NONE,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = graphics.graphicsQueueFamily(),
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL_KHR,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    const VkDependencyInfo ownershipDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &ownershipBarrier,
    };
    vkCmdPipelineBarrier2(commandBuffer, &ownershipDependency);
    requireVulkan(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer(CEF direct release)");
  }

  bool preparePendingDirectFrame() {
    if (!pendingFrame) {
      return false;
    }
    PendingFrame& pending = *pendingFrame;
    if (pending.prepared) {
      return true;
    }
    recordDirectAcquire(
        pending.slot->commandBuffer, pending.imported->source->image, pending.queueFamilyIndex,
        pending.producerOldLayout, pending.producerNewLayout
    );
    const VkSemaphoreSubmitInfo waitInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = pending.slot->acquireSemaphore,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    const VkCommandBufferSubmitInfo commandInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = pending.slot->commandBuffer,
    };
    const VkSemaphoreSubmitInfo signalInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = pending.slot->acquireCompleteSemaphore,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    const VkSubmitInfo2 submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &waitInfo,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &commandInfo,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signalInfo,
    };
    requireVulkan(
        vkQueueSubmit2(graphics.graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit2(CEF direct acquire)"
    );
    pending.prepared = true;
    pending.acquireCompletionWait = true;
    return true;
  }

  [[nodiscard]] std::optional<PendingRelease> releasePendingDirectFrame() {
    if (!pendingFrame) {
      return std::nullopt;
    }
    if (pendingFrame->outputGeneration == 0 || pendingFrame->contentSerial == 0) {
      throw std::runtime_error("pending CEF frame lost its output identity");
    }
    if (!preparePendingDirectFrame()) {
      return std::nullopt;
    }
    PendingFrame pending = *pendingFrame;
    FrameSlot& slot = *pending.slot;
    CachedImport& imported = *pending.imported;
    recordDirectRelease(slot.releaseCommandBuffer, imported.source->image);
    const std::uint64_t completionValue = ++nextCompletionValue;
    const VkCommandBufferSubmitInfo commandInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = slot.releaseCommandBuffer,
    };
    const VkSemaphoreSubmitInfo graphiteWaitInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = completionTimeline,
        .value = pending.graphiteCompletionValue,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    const VkSemaphoreSubmitInfo acquireWaitInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = slot.acquireCompleteSemaphore,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    std::array<VkSemaphoreSubmitInfo, 2> waits{};
    std::uint32_t waitCount = 0;
    if (pending.acquireCompletionWait) {
      waits[waitCount++] = acquireWaitInfo;
    }
    if (pending.graphiteCompletionValue != 0) {
      waits[waitCount++] = graphiteWaitInfo;
    }
    const std::array signals{
        VkSemaphoreSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = completionTimeline,
            .value = completionValue,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        },
        VkSemaphoreSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = slot.releaseSemaphore,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        },
    };
    const VkSubmitInfo2 submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = waitCount,
        .pWaitSemaphoreInfos = waitCount != 0 ? waits.data() : nullptr,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &commandInfo,
        .signalSemaphoreInfoCount = static_cast<std::uint32_t>(signals.size()),
        .pSignalSemaphoreInfos = signals.data(),
    };
    requireVulkan(
        vkQueueSubmit2(graphics.graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit2(CEF direct release)"
    );
    slot.completionValue = completionValue;
    imported.lastCompletionValue = completionValue;
    const VkSemaphoreGetFdInfoKHR releaseFdInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = slot.releaseSemaphore,
        .handleType = kSyncFdHandle,
    };
    int fd = -1;
    requireVulkan(getSemaphoreFd(graphics.device(), &releaseFdInfo, &fd), "vkGetSemaphoreFdKHR(CEF direct release)");
    ++lifetimeStats.releaseFenceFdsExported;
    if (pending.sampled) {
      ++lifetimeStats.directFramesSampled;
    } else {
      ++lifetimeStats.directFramesDiscarded;
    }
    pendingFrame.reset();
    return PendingRelease{
        .transportEpoch = pending.transportEpoch,
        .captureCounter = pending.captureCounter,
        .fd = fd,
    };
  }

  bool accept(const BorrowedDmabufFrame& frame) noexcept {
    NOCTALIA_TRACE_ZONE("CEF GPU frame bridge accept");
    std::optional<PendingRelease> release;
    try {
      std::unique_lock lock(mutex);
      const VkFormat candidateFormat = formatForFourcc(frame.fourcc);
      validate(frame, candidateFormat);
      if (frame.transportEpoch == 0) {
        throw std::runtime_error("CEF direct-sampling frame has no transport epoch");
      }
      if (frame.captureCounter < 0) {
        throw std::runtime_error("CEF direct-sampling frame has no capture counter");
      }
      if (frame.outputGeneration == 0 || frame.contentSerial == 0) {
        throw std::runtime_error("CEF direct-sampling frame has incomplete output identity");
      }
      if (frame.transportEpoch < lastTransportEpoch) {
        throw std::runtime_error("CEF direct-sampling frame has a stale transport epoch");
      }
      if (frame.transportEpoch == lastTransportEpoch
          && (frame.outputGeneration < lastOutputGeneration
              || (frame.outputGeneration == lastOutputGeneration && frame.contentSerial <= lastContentSerial))) {
        throw std::runtime_error("CEF direct-sampling frame identity is stale or reordered");
      }
      if (frame.transportEpoch > lastTransportEpoch) {
        lastOutputGeneration = 0;
        lastContentSerial = 0;
      }
      // A newer paint can supersede a frame before the scene draws it. It still
      // needs a balanced acquire/release so Viz can recycle it.
      // Keep the displayed DMA-BUF owned by Noctalia until this replacement
      // is rebound below. The scene may be redrawn for reasons unrelated to a
      // new CEF paint, so releasing immediately after its first sample would
      // leave the texture handle pointing at memory CEF is free to rewrite.
      release = releasePendingDirectFrame();
      FrameSlot& slot = acquireFrameSlot();
      CachedImport& source = cachedSource(frame, candidateFormat);
      (void)importAcquireFence(frame, slot.acquireSemaphore);
      pendingFrame = PendingFrame{
          .slot = &slot,
          .imported = &source,
          .transportEpoch = frame.transportEpoch,
          .captureCounter = frame.captureCounter,
          .outputGeneration = frame.outputGeneration,
          .contentSerial = frame.contentSerial,
          .queueFamilyIndex = frame.queueFamilyIndex,
          .producerOldLayout = static_cast<VkImageLayout>(frame.producerOldLayout),
          .producerNewLayout = static_cast<VkImageLayout>(frame.producerNewLayout),
      };
      lastOutputGeneration = frame.outputGeneration;
      lastContentSerial = frame.contentSerial;
      lastTransportEpoch = frame.transportEpoch;
      if (!textureHandle.valid()) {
        textureHandle = textures.adoptExternalImage(
            source.skImage, frame.width, frame.height, TextureFilter::Linear, synchronizationOwner
        );
        if (!textureHandle.valid()) {
          throw std::runtime_error("failed to register direct CEF image with texture manager");
        }
      } else if (!textures.rebindExternalImage(textureHandle, source.skImage, frame.width, frame.height)) {
        throw std::runtime_error("failed to rebind direct CEF image");
      }
      ++lifetimeStats.framesAccepted;
      ++lifetimeStats.directFramesStaged;
      NOCTALIA_TRACE_PLOT("CEF direct frames staged", static_cast<std::int64_t>(lifetimeStats.directFramesStaged));
      error.clear();
      lock.unlock();
      deliverRelease(std::move(release));
      return true;
    } catch (const std::exception& exception) {
      setError(exception.what());
      deliverRelease(std::move(release));
      return false;
    } catch (...) {
      setError("unknown failure while accepting CEF DMA-BUF");
      deliverRelease(std::move(release));
      return false;
    }
  }
};

CefGpuFrameBridge::CefGpuFrameBridge(
    GraphicsDevice& graphics, GraphiteTextureManager& textures, ReleaseFenceCallback releaseFenceCallback
)
    : m_impl(std::make_unique<Impl>(graphics, textures, std::move(releaseFenceCallback), this)) {}

CefGpuFrameBridge::~CefGpuFrameBridge() = default;

bool CefGpuFrameBridge::acceptFrame(const BorrowedDmabufFrame& frame) noexcept { return m_impl->accept(frame); }

bool CefGpuFrameBridge::prepareForGraphiteSampling(GraphiteSubmissionDependency& dependency) {
  std::scoped_lock lock(m_impl->mutex);
  try {
    if (!m_impl->preparePendingDirectFrame() || !m_impl->pendingFrame) {
      return false;
    }
    const Impl::PendingFrame& pending = *m_impl->pendingFrame;
    dependency.waitSemaphore = pending.acquireCompletionWait ? pending.slot->acquireCompleteSemaphore : VK_NULL_HANDLE;
    dependency.signalSemaphore = pending.slot->samplingCompleteSemaphore;
    return true;
  } catch (const std::exception& exception) {
    m_impl->setError(exception.what());
    return false;
  }
}

void CefGpuFrameBridge::finishGraphiteSampling(const GraphiteSubmissionDependency& dependency, bool submitted) {
  std::scoped_lock lock(m_impl->mutex);
  if (m_impl->pendingFrame
      && submitted
      && dependency.signalSemaphore == m_impl->pendingFrame->slot->samplingCompleteSemaphore) {
    Impl::PendingFrame& pending = *m_impl->pendingFrame;
    if (dependency.waitSemaphore == pending.slot->acquireCompleteSemaphore) {
      pending.acquireCompletionWait = false;
    }
    const std::uint64_t completionValue = ++m_impl->nextCompletionValue;
    const VkSemaphoreSubmitInfo waitInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = dependency.signalSemaphore,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    const VkSemaphoreSubmitInfo signalInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = m_impl->completionTimeline,
        .value = completionValue,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    const VkSubmitInfo2 submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &waitInfo,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signalInfo,
    };
    const VkResult result = vkQueueSubmit2(m_impl->graphics.graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
      m_impl->setError(std::format("vkQueueSubmit2(CEF Graphite completion) failed: {}", vulkanResultName(result)));
      return;
    }
    // Translate the exact binary semaphore signaled by Graphite into the
    // bridge timeline. One imported image can remain the live scene texture
    // across many redraws, so its eventual EXTERNAL release waits for the
    // newest Graphite submission that sampled it.
    pending.graphiteCompletionValue = completionValue;
    // Retain ownership of the currently displayed buffer. It is released when
    // acceptFrame atomically rebinds the texture to its replacement.
    pending.sampled = true;
  }
}

void CefGpuFrameBridge::discardPendingFrame() {
  std::optional<Impl::PendingRelease> release;
  try {
    {
      std::scoped_lock lock(m_impl->mutex);
      release = m_impl->releasePendingDirectFrame();
    }
  } catch (const std::exception& exception) {
    m_impl->setError(exception.what());
  } catch (...) {
    m_impl->setError("unknown failure while discarding CEF DMA-BUF");
  }
  m_impl->deliverRelease(std::move(release));
}

void CefGpuFrameBridge::abandonDevice() noexcept {
  std::scoped_lock lock(m_impl->mutex);
  m_impl->deviceAbandoned = true;
  m_impl->pendingFrame.reset();
}

void CefGpuFrameBridge::invalidate() {
  std::optional<Impl::PendingRelease> release;
  try {
    {
      std::scoped_lock lock(m_impl->mutex);
      release = m_impl->releasePendingDirectFrame();
      m_impl->waitForGraphiteUse();
      m_impl->releaseTexture();
      m_impl->importCache.clear();
    }
  } catch (const std::exception& exception) {
    m_impl->setError(exception.what());
  } catch (...) {
    m_impl->setError("unknown failure while invalidating CEF DMA-BUF bridge");
  }
  m_impl->deliverRelease(std::move(release));
}

TextureHandle CefGpuFrameBridge::texture() const noexcept { return m_impl->textureHandle; }
CefGpuFrameBridgeStats CefGpuFrameBridge::stats() const noexcept {
  std::scoped_lock lock(m_impl->mutex);
  return m_impl->lifetimeStats;
}
const std::string& CefGpuFrameBridge::lastError() const noexcept { return m_impl->error; }
