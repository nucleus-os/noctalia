#include "render/graphics_device.h"

#include "core/log.h"
#include "render/backend/graphite_texture_manager.h"
#include "render/vulkan/vulkan_result.h"

#include <wayland-client-core.h>
#include <vulkan/vulkan_wayland.h>

#include "include/core/SkRefCnt.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/vk/VulkanGraphiteContext.h"
#include "include/gpu/vk/VulkanBackendContext.h"
#include "include/gpu/vk/VulkanExtensions.h"
#include "include/gpu/vk/VulkanMemoryAllocator.h"
#include "src/gpu/GpuTypesPriv.h"
#include "src/gpu/vk/VulkanInterface.h"
#include "src/gpu/vk/vulkanmemoryallocator/VulkanAMDMemoryAllocator.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
  constexpr Logger kLog("vulkan");
  constexpr std::uint32_t kRequiredApiVersion = VK_API_VERSION_1_4;
  constexpr std::string_view kValidationLayer = "VK_LAYER_KHRONOS_validation";

  PFN_vkVoidFunction getVulkanProc(const char* name, VkInstance instance, VkDevice device) {
    return device != VK_NULL_HANDLE ? vkGetDeviceProcAddr(device, name) : vkGetInstanceProcAddr(instance, name);
  }

  bool containsExtension(std::span<const VkExtensionProperties> properties, std::string_view name) {
    return std::ranges::any_of(properties, [name](const VkExtensionProperties& property) {
      return name == property.extensionName;
    });
  }

  bool containsLayer(std::span<const VkLayerProperties> properties, std::string_view name) {
    return std::ranges::any_of(properties, [name](const VkLayerProperties& property) {
      return name == property.layerName;
    });
  }

  template <typename T, typename Enumerate>
  std::vector<T> enumerateVulkan(std::string_view operation, Enumerate&& enumerate) {
    std::uint32_t count = 0;
    requireVulkan(enumerate(&count, nullptr), operation);
    std::vector<T> values(count);
    VkResult result = enumerate(&count, values.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
      requireVulkan(result, operation);
    }
    values.resize(count);
    return values;
  }

  VKAPI_ATTR VkBool32 VKAPI_CALL validationMessage(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT /*types*/,
      const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData
  ) {
    auto* counts = static_cast<std::array<std::atomic<std::uint64_t>, 2>*>(userData);
    const char* message = data != nullptr && data->pMessage != nullptr ? data->pMessage : "unknown validation error";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
      if (counts != nullptr) {
        (*counts)[0].fetch_add(1);
      }
      kLog.error("validation: {}", message);
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
      if (counts != nullptr) {
        (*counts)[1].fetch_add(1);
      }
      kLog.warn("validation: {}", message);
    } else {
      kLog.debug("validation: {}", message);
    }
    return VK_FALSE;
  }

  VkDebugUtilsMessengerCreateInfoEXT debugMessengerInfo(void* userData) {
    return VkDebugUtilsMessengerCreateInfoEXT{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = validationMessage,
        .pUserData = userData,
    };
  }

  std::vector<const char*> cStrings(const std::vector<std::string>& strings) {
    std::vector<const char*> result;
    result.reserve(strings.size());
    for (const auto& value : strings) {
      result.push_back(value.c_str());
    }
    return result;
  }
}

struct GraphicsDevice::GraphiteState {
  skgpu::VulkanExtensions extensions;
  sk_sp<skgpu::VulkanInterface> interface;
  sk_sp<skgpu::VulkanMemoryAllocator> allocator;
  std::unique_ptr<skgpu::graphite::Context> context;
  std::unique_ptr<skgpu::graphite::Recorder> recorder;
};

GraphicsDevice::GraphicsDevice() = default;

GraphicsDevice::~GraphicsDevice() { cleanup(); }

bool GraphicsDevice::valid() const noexcept {
  return m_device != VK_NULL_HANDLE && m_graphite != nullptr && m_graphite->context != nullptr
      && m_graphite->recorder != nullptr;
}

skgpu::graphite::Context* GraphicsDevice::graphiteContext() const noexcept {
  return m_graphite != nullptr ? m_graphite->context.get() : nullptr;
}

skgpu::graphite::Recorder* GraphicsDevice::recorder() const noexcept {
  return m_graphite != nullptr ? m_graphite->recorder.get() : nullptr;
}

GraphiteTextureManager& GraphicsDevice::textureManager() {
  if (!valid()) {
    throw std::runtime_error("GraphicsDevice texture manager requested before initialization");
  }
  if (m_textureManager == nullptr) {
    m_textureManager = std::make_unique<GraphiteTextureManager>(*this);
  }
  return *m_textureManager;
}

GraphicsDeviceIdentity GraphicsDevice::identity() const {
  if (m_physicalDevice == VK_NULL_HANDLE) {
    throw std::runtime_error("GraphicsDevice identity requested before device selection");
  }
  VkPhysicalDeviceDrmPropertiesEXT drmProperties{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
  };
  VkPhysicalDeviceIDProperties idProperties{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
      .pNext = &drmProperties,
  };
  VkPhysicalDeviceProperties2 properties{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &idProperties,
  };
  vkGetPhysicalDeviceProperties2(m_physicalDevice, &properties);
  if (drmProperties.hasRender == VK_FALSE) {
    throw std::runtime_error("selected Vulkan device does not expose a DRM render node");
  }

  GraphicsDeviceIdentity result;
  std::ranges::copy(idProperties.deviceUUID, result.uuid.begin());
  result.drmRenderNode = "/dev/dri/renderD" + std::to_string(drmProperties.renderMinor);
  return result;
}

void GraphicsDevice::configureExtensionContract() {
  m_instanceExtensions = {
      VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
      VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
      VK_KHR_SURFACE_EXTENSION_NAME,
      VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
      VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
  };
  if (m_requirements.validation) {
    m_instanceExtensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }

  m_deviceExtensions = {
      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
      VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
      VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
      VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
      VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
      VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
      VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
      VK_KHR_MAINTENANCE_3_EXTENSION_NAME,
  };
  if (m_requirements.cefExternalMemory) {
    m_deviceExtensions.insert(
        m_deviceExtensions.end(),
        {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
         VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
         VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
         VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME}
    );
  }
}

void GraphicsDevice::initialize(
    wl_display* display, GraphicsDeviceRequirements requirements,
    wp_presentation* presentation, std::int32_t presentationClockId
) {
  cleanup();
  m_validationMessages[0].store(0);
  m_validationMessages[1].store(0);
  if (display == nullptr) {
    throw std::runtime_error("GraphicsDevice requires an active Wayland display");
  }
  m_waylandDisplay = display;
  m_waylandPresentation = presentation;
  m_presentationClockId = presentationClockId;
  m_requirements = requirements;
  configureExtensionContract();
  try {
    createInstance(requirements.validation);
    selectPhysicalDevice();
    createLogicalDevice();
    createGraphite();
  } catch (...) {
    cleanup();
    throw;
  }
}

void GraphicsDevice::createInstance(bool validation) {
  std::uint32_t loaderVersion = VK_API_VERSION_1_0;
  requireVulkan(vkEnumerateInstanceVersion(&loaderVersion), "vkEnumerateInstanceVersion");
  if (loaderVersion < kRequiredApiVersion) {
    throw std::runtime_error(std::format(
        "Vulkan 1.4 is required; loader exposes {}.{}.{}", VK_API_VERSION_MAJOR(loaderVersion),
        VK_API_VERSION_MINOR(loaderVersion), VK_API_VERSION_PATCH(loaderVersion)
    ));
  }

  const auto availableExtensions = enumerateVulkan<VkExtensionProperties>(
      "vkEnumerateInstanceExtensionProperties",
      [](std::uint32_t* count, VkExtensionProperties* properties) {
        return vkEnumerateInstanceExtensionProperties(nullptr, count, properties);
      }
  );
  for (const auto& extension : m_instanceExtensions) {
    if (!containsExtension(availableExtensions, extension)) {
      throw std::runtime_error("required Vulkan instance extension is unavailable: " + extension);
    }
  }

  std::vector<const char*> layers;
  if (validation) {
    const auto availableLayers = enumerateVulkan<VkLayerProperties>(
        "vkEnumerateInstanceLayerProperties",
        [](std::uint32_t* count, VkLayerProperties* properties) {
          return vkEnumerateInstanceLayerProperties(count, properties);
        }
    );
    if (!containsLayer(availableLayers, kValidationLayer)) {
      throw std::runtime_error("Vulkan validation was requested but VK_LAYER_KHRONOS_validation is unavailable");
    }
    layers.push_back(kValidationLayer.data());
  }

  const auto extensionNames = cStrings(m_instanceExtensions);
  const VkApplicationInfo appInfo{
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "Noctalia",
      .applicationVersion = VK_MAKE_API_VERSION(0, 5, 0, 0),
      .pEngineName = "Noctalia Graphite",
      .engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
      .apiVersion = kRequiredApiVersion,
  };
  const VkValidationFeatureEnableEXT validationFeature =
      VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
  VkValidationFeaturesEXT validationFeatures{
      .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
      .enabledValidationFeatureCount = validation ? 1U : 0U,
      .pEnabledValidationFeatures = validation ? &validationFeature : nullptr,
  };
  VkDebugUtilsMessengerCreateInfoEXT debugInfo = debugMessengerInfo(&m_validationMessages);
  debugInfo.pNext = validation ? &validationFeatures : nullptr;
  const VkInstanceCreateInfo createInfo{
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pNext = validation ? &debugInfo : nullptr,
      .pApplicationInfo = &appInfo,
      .enabledLayerCount = static_cast<std::uint32_t>(layers.size()),
      .ppEnabledLayerNames = layers.data(),
      .enabledExtensionCount = static_cast<std::uint32_t>(extensionNames.size()),
      .ppEnabledExtensionNames = extensionNames.data(),
  };
  requireVulkan(vkCreateInstance(&createInfo, nullptr, &m_instance), "vkCreateInstance");

  if (validation) {
    // VkValidationFeaturesEXT is valid in the instance-create chain, not in
    // the vkCreateDebugUtilsMessengerEXT chain used below.
    debugInfo.pNext = nullptr;
    const auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT")
    );
    if (createMessenger == nullptr) {
      throw std::runtime_error("vkCreateDebugUtilsMessengerEXT is unavailable");
    }
    requireVulkan(createMessenger(m_instance, &debugInfo, nullptr, &m_debugMessenger), "vkCreateDebugUtilsMessengerEXT");
    kLog.info("enabled VK_LAYER_KHRONOS_validation with synchronization validation");
  }
}

bool GraphicsDevice::supportsDeviceExtensions(
    VkPhysicalDevice device, std::span<const char* const> extensions
) {
  std::uint32_t count = 0;
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) {
    return false;
  }
  std::vector<VkExtensionProperties> properties(count);
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, properties.data()) != VK_SUCCESS) {
    return false;
  }
  return std::ranges::all_of(extensions, [&properties](const char* extension) {
    return containsExtension(properties, extension);
  });
}

void GraphicsDevice::selectPhysicalDevice() {
  const auto devices = enumerateVulkan<VkPhysicalDevice>(
      "vkEnumeratePhysicalDevices", [this](std::uint32_t* count, VkPhysicalDevice* values) {
        return vkEnumeratePhysicalDevices(m_instance, count, values);
      }
  );
  const auto requiredExtensions = cStrings(m_deviceExtensions);

  for (VkPhysicalDevice device : devices) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    if (properties.apiVersion < kRequiredApiVersion || !supportsDeviceExtensions(device, requiredExtensions)) {
      continue;
    }

    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
    };
    VkPhysicalDeviceVulkan11Features features11{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = &maintenance,
    };
    VkPhysicalDeviceVulkan12Features features12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &features11,
    };
    VkPhysicalDeviceVulkan13Features features13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &features12,
    };
    VkPhysicalDeviceFeatures2 features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &features13,
    };
    vkGetPhysicalDeviceFeatures2(device, &features);
    if (features12.timelineSemaphore == VK_FALSE || features11.samplerYcbcrConversion == VK_FALSE
        || features13.synchronization2 == VK_FALSE || maintenance.swapchainMaintenance1 == VK_FALSE) {
      continue;
    }

    std::uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
    for (std::uint32_t family = 0; family < familyCount; ++family) {
      if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
        continue;
      }
      if (vkGetPhysicalDeviceWaylandPresentationSupportKHR(device, family, m_waylandDisplay) == VK_FALSE) {
        continue;
      }
      m_physicalDevice = device;
      m_graphicsQueueFamily = family;
      kLog.info(
          "selected compositor-presenting Vulkan device '{}' (API {}.{}.{}, queue family {})",
          properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
          VK_API_VERSION_PATCH(properties.apiVersion), family
      );
      return;
    }
  }
  throw std::runtime_error(
      "no Vulkan 1.4 device has one queue family supporting both graphics and presentation to this Wayland display"
  );
}

void GraphicsDevice::createLogicalDevice() {
  float priority = 1.0f;
  const VkDeviceQueueCreateInfo queueInfo{
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = m_graphicsQueueFamily,
      .queueCount = 1,
      .pQueuePriorities = &priority,
  };
  VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
      .swapchainMaintenance1 = VK_TRUE,
  };
  VkPhysicalDeviceVulkan11Features features11{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
      .pNext = &maintenance,
      .samplerYcbcrConversion = VK_TRUE,
  };
  VkPhysicalDeviceVulkan12Features features12{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .pNext = &features11,
      .timelineSemaphore = VK_TRUE,
  };
  VkPhysicalDeviceVulkan13Features features13{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .pNext = &features12,
      .synchronization2 = VK_TRUE,
  };
  VkPhysicalDeviceFeatures2 features{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &features13,
  };
  const auto extensionNames = cStrings(m_deviceExtensions);
  const VkDeviceCreateInfo createInfo{
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &features,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queueInfo,
      .enabledExtensionCount = static_cast<std::uint32_t>(extensionNames.size()),
      .ppEnabledExtensionNames = extensionNames.data(),
  };
  requireVulkan(vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device), "vkCreateDevice");
  vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
  if (m_graphicsQueue == VK_NULL_HANDLE) {
    throw std::runtime_error("vkGetDeviceQueue returned a null graphics queue");
  }
  m_cefExternalMemoryEnabled = m_requirements.cefExternalMemory;
}

void GraphicsDevice::createGraphite() {
  m_graphite = std::make_unique<GraphiteState>();
  const auto instanceExtensionNames = cStrings(m_instanceExtensions);
  const auto deviceExtensionNames = cStrings(m_deviceExtensions);
  const skgpu::VulkanGetProc getProc = getVulkanProc;
  m_graphite->extensions.init(
      getProc, m_instance, m_physicalDevice, static_cast<std::uint32_t>(instanceExtensionNames.size()),
      instanceExtensionNames.data(), static_cast<std::uint32_t>(deviceExtensionNames.size()),
      deviceExtensionNames.data()
  );
  m_graphite->interface = sk_make_sp<skgpu::VulkanInterface>(
      getProc, m_instance, m_device, kRequiredApiVersion, kRequiredApiVersion, &m_graphite->extensions
  );
  if (!m_graphite->interface->validate(kRequiredApiVersion, kRequiredApiVersion, &m_graphite->extensions)) {
    throw std::runtime_error("Skia rejected the Vulkan interface contract");
  }
  m_graphite->allocator = skgpu::VulkanAMDMemoryAllocator::Make(
      m_instance, m_physicalDevice, m_device, kRequiredApiVersion, &m_graphite->extensions,
      m_graphite->interface.get(), skgpu::ThreadSafe::kYes
  );
  if (m_graphite->allocator == nullptr) {
    throw std::runtime_error("Skia VulkanAMDMemoryAllocator creation failed");
  }

  skgpu::VulkanBackendContext backend;
  backend.fInstance = m_instance;
  backend.fPhysicalDevice = m_physicalDevice;
  backend.fDevice = m_device;
  backend.fQueue = m_graphicsQueue;
  backend.fGraphicsQueueIndex = m_graphicsQueueFamily;
  backend.fMaxAPIVersion = kRequiredApiVersion;
  backend.fVkExtensions = &m_graphite->extensions;
  backend.fMemoryAllocator = m_graphite->allocator;
  backend.fGetProc = getProc;
  backend.fProtectedContext = skgpu::Protected::kNo;

  skgpu::graphite::ContextOptions contextOptions;
  m_graphite->context = skgpu::graphite::ContextFactory::MakeVulkan(backend, contextOptions);
  if (m_graphite->context == nullptr) {
    throw std::runtime_error("Skia Graphite Vulkan context creation failed");
  }
  skgpu::graphite::RecorderOptions recorderOptions;
  recorderOptions.fRequireOrderedRecordings = true;
  m_graphite->recorder = m_graphite->context->makeRecorder(recorderOptions);
  if (m_graphite->recorder == nullptr) {
    throw std::runtime_error("Skia Graphite recorder creation failed");
  }
  kLog.info("initialized native Skia Graphite Vulkan context");
}

void GraphicsDevice::destroyDeviceObjects() {
  if (m_device == VK_NULL_HANDLE) {
    m_textureManager.reset();
    m_graphite.reset();
    return;
  }
  // Teardown and the one-shot device-loss rebuild are the only paths allowed
  // to idle the whole device.
  (void)vkDeviceWaitIdle(m_device);
  if (m_graphite != nullptr && m_graphite->context != nullptr) {
    m_graphite->context->checkAsyncWorkCompletion();
  }
  // Texture destruction enqueues recorder finish callbacks which drop the
  // SkImage references and delete their BackendTextures. Drain that retirement
  // recording explicitly: recorder destruction alone does not guarantee that
  // unsnapped finish callbacks run before vkDestroyDevice.
  m_textureManager.reset();
  if (m_graphite != nullptr && m_graphite->recorder != nullptr && m_graphite->context != nullptr) {
    auto retirementRecording = m_graphite->recorder->snap();
    if (retirementRecording != nullptr) {
      skgpu::graphite::InsertRecordingInfo insertInfo;
      insertInfo.fRecording = retirementRecording.get();
      (void)m_graphite->context->insertRecording(insertInfo);
      (void)m_graphite->context->submit(skgpu::graphite::SyncToCpu::kYes);
      m_graphite->context->checkAsyncWorkCompletion();
    }
  }
  m_graphite.reset();
  vkDestroyDevice(m_device, nullptr);
  m_device = VK_NULL_HANDLE;
  m_graphicsQueue = VK_NULL_HANDLE;
  m_physicalDevice = VK_NULL_HANDLE;
  m_graphicsQueueFamily = UINT32_MAX;
  m_cefExternalMemoryEnabled = false;
}

void GraphicsDevice::destroyInstanceObjects() {
  if (m_instance == VK_NULL_HANDLE) {
    return;
  }
  if (m_debugMessenger != VK_NULL_HANDLE) {
    const auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT")
    );
    if (destroyMessenger != nullptr) {
      destroyMessenger(m_instance, m_debugMessenger, nullptr);
    }
    m_debugMessenger = VK_NULL_HANDLE;
  }
  vkDestroyInstance(m_instance, nullptr);
  m_instance = VK_NULL_HANDLE;
}

void GraphicsDevice::cleanup() {
  destroyDeviceObjects();
  destroyInstanceObjects();
  m_waylandDisplay = nullptr;
  m_waylandPresentation = nullptr;
  m_presentationClockId = -1;
  m_instanceExtensions.clear();
  m_deviceExtensions.clear();
}

void GraphicsDevice::rebuild() {
  if (m_waylandDisplay == nullptr) {
    throw std::runtime_error("cannot rebuild an uninitialized GraphicsDevice");
  }
  wl_display* display = m_waylandDisplay;
  wp_presentation* presentation = m_waylandPresentation;
  const std::int32_t presentationClockId = m_presentationClockId;
  const auto requirements = m_requirements;
  destroyDeviceObjects();
  destroyInstanceObjects();
  ++m_generation;
  m_waylandDisplay = display;
  m_waylandPresentation = presentation;
  m_presentationClockId = presentationClockId;
  configureExtensionContract();
  createInstance(requirements.validation);
  selectPhysicalDevice();
  createLogicalDevice();
  createGraphite();
}
