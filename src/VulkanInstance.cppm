module;

#include "imgui.h"
#include "macros.hpp"
#include "primitive_types.hpp"
#include <SDL3/SDL_vulkan.h>

export module vulkan_app:VulkanInstance;

import vulkan_hpp;
import std;

namespace {
constexpr bool enable_validation_layers = ENABLE_VALIDATION;
constexpr const char *validationLayerName = "VK_LAYER_KHRONOS_validation";
constexpr std::array layers = {validationLayerName};
} // namespace

vk::Bool32 debugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                         vk::DebugUtilsMessageTypeFlagsEXT messageType,
                         const vk::DebugUtilsMessengerCallbackDataEXT *pCallbackData,
                         void * /*unused*/) {
  std::println("Validation Layer: {}\n", pCallbackData->pMessage);
  return vk::False;
}

export struct VulkanInstance {
  vk::raii::Instance instance{nullptr};
  vk::raii::DebugUtilsMessengerEXT debugMessenger{nullptr};

private:
  static bool IsExtensionAvailable(const ImVector<vk::ExtensionProperties> &properties,
                                   const char *extension) {
    for (decltype(auto) property : properties) {
      if (std::strcmp(property.extensionName, extension) == 0) {
        return true;
      }
    }
    return false;
  }

  static bool checkValidationLayerSupport(const vk::raii::Context &context) {
    const auto availableLayers = context.enumerateInstanceLayerProperties();
    return std::ranges::all_of(layers, [&](const char *layer) {
      return std::ranges::any_of(availableLayers, [&](const auto &availableLayer) {
        return std::strcmp(layer, availableLayer.layerName) == 0;
      });
    });
  }

public:
  [[nodiscard]] VkInstance get_C_handle() const { return *instance; }

  // Access via ->
  auto operator->() { return &instance; }
  auto operator->() const { return &instance; }
  // Implicit conversion to underlying type
  explicit operator vk::raii::Instance &() { return instance; }
  explicit operator const vk::raii::Instance &() const { return instance; }

  [[nodiscard]] std::expected<void, std::string> setupDebugMessenger() {
    if constexpr (!enable_validation_layers) {
      return {};
    }
    if (auto expected = instance.createDebugUtilsMessengerEXT({
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

  [[nodiscard]] std::expected<void, std::string> create() {
    vk::raii::Context context;
    if constexpr (enable_validation_layers) {
      if (!checkValidationLayerSupport(context)) {
        return std::unexpected("Validation layers requested but not available!");
      }
    }

    vk::ApplicationInfo appInfo{
        .pApplicationName = "Vulkan App",
        .applicationVersion = 0,
        .pEngineName = "No Engine",
        .engineVersion = 0,
        .apiVersion = vk::makeApiVersion(0, 1, 3, 0),
    };

    std::vector<const char *> extensions;
    {
      u32 sdl_extensions_count = 0;
      const auto *sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_extensions_count);
      for (u32 n = 0; n < sdl_extensions_count; n++) {
        extensions.push_back(sdl_extensions[n]);
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

    if constexpr (enable_validation_layers) {
      extensions.push_back(vk::EXTDebugUtilsExtensionName);
    }

    vk::InstanceCreateInfo createInfo{
        .pApplicationInfo = &appInfo,
        .enabledExtensionCount = static_cast<u32>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };

    if constexpr (!enable_validation_layers) {
      createInfo.enabledLayerCount = static_cast<u32>(layers.size());
      createInfo.ppEnabledLayerNames = layers.data();
    }

    if (auto expected = context.createInstance(createInfo); expected) {
      instance = std::move(*expected);
      std::println("Vulkan instance created successfully!");
      return {};
    } else {
      return std::unexpected("Failed to create Vulkan instance: " +
                             vk::to_string(expected.error()));
    }
  }
};
