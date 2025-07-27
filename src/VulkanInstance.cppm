module;

#include "imgui.h"
#include "macros.hpp"
#include "primitive_types.hpp"
#include <SDL3/SDL_vulkan.h>

export module vulkan_app:VulkanInstance;

import vulkan_hpp;
import std;

namespace {
constexpr bool ENABLE_VALIDATION_LAYERS = ENABLE_VALIDATION;
constexpr const char *VALIDATION_LAYER_NAME = "VK_LAYER_KHRONOS_validation";
constexpr std::array LAYERS = {VALIDATION_LAYER_NAME};

bool isExtensionAvailable(const ImVector<vk::ExtensionProperties> &properties,
                          const char *extension) {
  for (decltype(auto) property : properties) {
    if (std::strcmp(property.extensionName, extension) == 0) {
      return true;
    }
  }
  return false;
}

bool checkValidationLayerSupport(const vk::raii::Context &context) {
  const auto AVAILABLE_LAYERS = context.enumerateInstanceLayerProperties();
  return std::ranges::all_of(LAYERS, [&](const char *layer) {
    return std::ranges::any_of(AVAILABLE_LAYERS, [&](const auto &availableLayer) {
      return std::strcmp(layer, availableLayer.layerName) == 0;
    });
  });
}
} // namespace

vk::Bool32 debugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                         vk::DebugUtilsMessageTypeFlagsEXT messageType,
                         const vk::DebugUtilsMessengerCallbackDataEXT *pCallbackData,
                         void * /*unused*/) {
  std::println("Validation Layer: {}\n", pCallbackData->pMessage);
  return vk::False;
}

export class VulkanInstance {
  vk::raii::Instance m_instance{nullptr};

  [[nodiscard]] std::expected<void, std::string> setupDebugMessenger() {
    if constexpr (!ENABLE_VALIDATION_LAYERS) {
      return {};
    }

    vk::raii::DebugUtilsMessengerEXT debugMessenger{nullptr};

    if (auto expected = m_instance.createDebugUtilsMessengerEXT({
            .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                               vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
            .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                           vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                           vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
            .pfnUserCallback = debugCallback,
        });
        expected) {
      debugMessenger = std::move(*expected);
      return {};
    } else {
      return std::unexpected("Failed to create debug messenger: " +
                             vk::to_string(expected.error()));
    }
  }

public:
  [[nodiscard]] VkInstance getCHandle() const { return *m_instance; }

  // Access via ->
  auto operator->() { return &m_instance; }
  auto operator->() const { return &m_instance; }
  // Implicit conversion to underlying type
  operator vk::raii::Instance &() { return m_instance; }
  operator const vk::raii::Instance &() const { return m_instance; }

  [[nodiscard]] std::expected<void, std::string> create() {
    vk::raii::Context context;
    if constexpr (ENABLE_VALIDATION_LAYERS) {
      if (!checkValidationLayerSupport(context)) {
        return std::unexpected("Validation layers requested but not available!");
      }
    }

    vk::ApplicationInfo appInfo{
        .pApplicationName = "Vulkan App",
        .applicationVersion = 0,
        .pEngineName = "No Engine",
        .engineVersion = 0,
        .apiVersion = vk::makeApiVersion(0, 1, 4, 0),
    };

    std::vector<const char *> extensions;
    {
      u32 sdlExtensionsCount = 0;
      const auto *sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionsCount);
      for (u32 n = 0; n < sdlExtensionsCount; n++) {
        extensions.push_back(sdlExtensions[n]);
      }
    }
    extensions.push_back(vk::KHRSurfaceExtensionName);

#ifdef VK_USE_PLATFORM_WIN32_KHR
    extensions.push_back(vk::KHRWin32SurfaceExtensionName);
#elif VK_USE_PLATFORM_XLIB_KHR
    extensions.push_back(vk::KHRXlibSurfaceExtensionName);
#elif VK_USE_PLATFORM_WAYLAND_KHR
    extensions.push_back(vk::KHRWaylandSurfaceExtensionName);
#elif VK_USE_PLATFORM_METAL_EXT
    extensions.push_back(vk::EXTMetalSurfaceExtensionName);
#elif VK_USE_PLATFORM_XCB_KHR
    extensions.push_back(vk::KHRXcbSurfaceExtensionName);
#endif

    if constexpr (ENABLE_VALIDATION_LAYERS) {
      extensions.push_back(vk::EXTDebugUtilsExtensionName);
    }

    vk::InstanceCreateInfo createInfo{
        .pApplicationInfo = &appInfo,
        .enabledExtensionCount = static_cast<u32>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };

    if constexpr (!ENABLE_VALIDATION_LAYERS) {
      createInfo.enabledLayerCount = static_cast<u32>(LAYERS.size());
      createInfo.ppEnabledLayerNames = LAYERS.data();
    }

    if (auto expected = context.createInstance(createInfo); expected) {
      m_instance = std::move(*expected);
      std::println("Vulkan instance created successfully!");
      return {};
    } else {
      return std::unexpected("Failed to create Vulkan instance: " +
                             vk::to_string(expected.error()));
    }

    if (auto expected = setupDebugMessenger(); !expected) {
      return expected;
    }
  }
};
