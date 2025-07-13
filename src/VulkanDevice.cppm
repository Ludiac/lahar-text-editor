module;

#include "imgui_impl_vulkan.h"
#include "macros.hpp"
#include "primitive_types.hpp"

export module vulkan_app:VulkanDevice;

import vulkan_hpp;
import std;
import vk_mem_alloc_hpp;
import :VMA;
import :utils;

export struct BufferResources {
  vk::raii::Buffer buffer{nullptr};
  vk::raii::DeviceMemory memory{nullptr};
};

export struct VulkanDevice {
private:
  vk::raii::PhysicalDevice physicalDevice_{nullptr};
  vk::raii::Device device_{nullptr};
  const vk::raii::Instance &instance_;
  vma::Allocator vmaAllocator_;
  vk::raii::CommandPool transientCommandPool_{nullptr};

public:
  vk::PhysicalDeviceLimits limits;
  u32 queueFamily_ = (u32)-1;
  vk::raii::Queue queue_{nullptr};
  vk::raii::DescriptorPool descriptorPool_{nullptr};

  VulkanDevice(const vk::raii::Instance &instance) : instance_(instance) {}

private:
  static u32 selectQueueFamilyIndex(const vk::raii::PhysicalDevice &physical_device) {
    auto queue_family_properties = physical_device.getQueueFamilyProperties();

    for (u32 i = 0; i < queue_family_properties.size(); i++) {
      if (queue_family_properties[i].queueFlags & vk::QueueFlagBits::eGraphics) {
        return i;
      }
    }

    return (u32)-1;
  }

public:
  [[nodiscard]] const vk::raii::Device &logical() const { return device_; }
  [[nodiscard]] const vk::raii::PhysicalDevice &physical() const { return physicalDevice_; }
  [[nodiscard]] const vma::Allocator &allocator() const { return vmaAllocator_; }

  ~VulkanDevice() { vmaAllocator_.destroy(); }

  ImGui_ImplVulkan_InitInfo init_info() {
    return ImGui_ImplVulkan_InitInfo{
        .PhysicalDevice = *physicalDevice_,
        .Device = *device_,
        .QueueFamily = queueFamily_,
        .Queue = *queue_,
        .DescriptorPool = *descriptorPool_,
    };
  }

  [[nodiscard]] std::expected<void, std::string> pickPhysicalDevice() {
    auto expected = instance_.enumeratePhysicalDevices();
    if (expected) {
      if (expected->empty()) {
        return std::unexpected("No Vulkan-compatible physical devices found!");
      }
      physicalDevice_ = std::move(expected->front());
      limits = physicalDevice_.getProperties().limits;
      return {};
    }
    return std::unexpected("Failed to enumerate physical devices: " +
                           vk::to_string(expected.error()));
  }

  [[nodiscard]] std::expected<void, std::string> createLogicalDevice() {
    queueFamily_ = selectQueueFamilyIndex(physicalDevice_);
    if (queueFamily_ == (u32)-1) {
      return std::unexpected("Failed to select queue family index");
    }

    std::vector<const char *> deviceExtensions;
    deviceExtensions.push_back(vk::KHRSwapchainExtensionName);
    deviceExtensions.push_back(vk::KHRMapMemory2ExtensionName);

    u32 properties_count;
    std::vector<vk::ExtensionProperties> properties =
        physicalDevice_.enumerateDeviceExtensionProperties();

    const float queue_priority[] = {1.0};
    vk::DeviceQueueCreateInfo queue_info[1] = {};
    queue_info[0].queueFamilyIndex = queueFamily_;
    queue_info[0].queueCount = 1;
    queue_info[0].pQueuePriorities = queue_priority;

    vk::PhysicalDeviceFeatures enabledFeatures{};
    vk::PhysicalDeviceVulkan11Features enabledFeatures11{
        .shaderDrawParameters = vk::True,
    };
    enabledFeatures.samplerAnisotropy = vk::True;
    if (auto expected = physicalDevice_.createDevice({
            .pNext = &enabledFeatures11,
            .queueCreateInfoCount = sizeof(queue_info) / sizeof(queue_info[0]),
            .pQueueCreateInfos = queue_info,
            .enabledExtensionCount = static_cast<u32>(deviceExtensions.size()),
            .ppEnabledExtensionNames = deviceExtensions.data(),
            .pEnabledFeatures = &enabledFeatures,
        });
        expected) {
      device_ = std::move(*expected);
    } else {
      return std::unexpected(std::format("error with device {}", vk::to_string(expected.error())));
    }
    if (auto expected = device_.getQueue(queueFamily_, 0); expected) {
      queue_ = std::move(*expected);
    } else {
      return std::unexpected(std::format("error with queue {}", vk::to_string(expected.error())));
    }

    vk::CommandPoolCreateInfo poolCreateInfo{.flags =
                                                 vk::CommandPoolCreateFlagBits::eTransient |
                                                 vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                             .queueFamilyIndex = queueFamily_};
    auto transientPoolResult = device_.createCommandPool(poolCreateInfo);
    if (!transientPoolResult) {
      return std::unexpected("Failed to create transient command pool: " +
                             vk::to_string(transientPoolResult.error()));
    }
    transientCommandPool_ = std::move(transientPoolResult.value());

    if (auto expected = createVmaAllocator(instance_, physicalDevice_, device_); expected) {
      vmaAllocator_ = *expected;
      return {};
    }
    return std::unexpected(std::format("Failed to create Vma allocator:"));
  }

  [[nodiscard]] std::expected<void, std::string> createDescriptorPool(u32 imageCountBasedFactor) {
    if (!*device_) {
      return std::unexpected("VulkanDevice::createDescriptorPool: Logical device is null. Call "
                             "createLogicalDevice first.");
    }

    u32 app_uniform_buffers = imageCountBasedFactor;
    u32 app_dynamic_uniform_buffers = imageCountBasedFactor;
    u32 app_combined_image_samplers = imageCountBasedFactor;

    std::vector<vk::DescriptorPoolSize> pool_sizes = {
        {.type = vk::DescriptorType::eUniformBuffer, .descriptorCount = app_uniform_buffers},
        {.type = vk::DescriptorType::eUniformBufferDynamic,
         .descriptorCount = app_dynamic_uniform_buffers},
        {.type = vk::DescriptorType::eCombinedImageSampler,
         .descriptorCount =
             app_combined_image_samplers + IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE},
        {.type = vk::DescriptorType::eStorageBuffer, .descriptorCount = 30},
        {.type = vk::DescriptorType::eStorageBufferDynamic, .descriptorCount = 10},
    };

    u32 application_max_sets = imageCountBasedFactor * 2;

    u32 imgui_estimated_sets = 10;

    vk::DescriptorPoolCreateInfo pool_info{.flags =
                                               vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
                                           .maxSets = application_max_sets + imgui_estimated_sets,
                                           .poolSizeCount = static_cast<u32>(pool_sizes.size()),
                                           .pPoolSizes = pool_sizes.data()};

    if (*descriptorPool_) {
      // Consider device.waitIdle() here if descriptor sets from the old pool might still be in use.
      // For simplicity, assuming this is called during setup or a controlled resize.
      descriptorPool_.clear();
    }

    auto poolResult = device_.createDescriptorPool(pool_info);
    if (!poolResult) {
      return std::unexpected(
          std::format("VulkanDevice::createDescriptorPool: Failed to create descriptor pool: {} - "
                      "MaxSets: {}, PoolSizes: UBOs({}), DynUBOs({}), Samplers({})",
                      vk::to_string(poolResult.error()), pool_info.maxSets,
                      (pool_sizes.size() > 0 ? pool_sizes[0].descriptorCount : 0),
                      (pool_sizes.size() > 1 ? pool_sizes[1].descriptorCount : 0),
                      (pool_sizes.size() > 2 ? pool_sizes[2].descriptorCount : 0)));
    }
    descriptorPool_ = std::move(poolResult.value());
    return {};
  }

  [[nodiscard]] std::expected<BufferResources, std::string>
  createBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage,
               vk::MemoryPropertyFlags memoryProperties) NOEXCEPT {
    BufferResources resources;

    if (auto buf = device_.createBuffer(
            {.size = size, .usage = usage, .sharingMode = vk::SharingMode::eExclusive})) {
      resources.buffer = std::move(*buf);
    } else {
      return std::unexpected("Buffer creation failed: " + vk::to_string(buf.error()));
    }

    auto memReqs = device_.getBufferMemoryRequirements2({.buffer = resources.buffer});
    u32 memType;
    if (auto memTypeExp = findMemoryType(physicalDevice_, memReqs.memoryRequirements.memoryTypeBits,
                                         memoryProperties)) {
      memType = *memTypeExp;
    } else {
      return std::unexpected(memTypeExp.error());
    }

    if (auto mem = device_.allocateMemory(
            {.allocationSize = memReqs.memoryRequirements.size, .memoryTypeIndex = memType})) {
      resources.memory = std::move(*mem);
    } else {
      return std::unexpected("Memory allocation failed: " + vk::to_string(mem.error()));
    }

    return resources;
  }

  [[nodiscard]] std::expected<VmaBuffer, std::string>
  createBufferVMA(const vk::BufferCreateInfo &bufferCreateInfo,
                  const vma::AllocationCreateInfo &allocationCreateInfo) NOEXCEPT {
    if (!vmaAllocator_) {
      return std::unexpected("VMA Allocator not initialized in VulkanDevice::createBufferVMA.");
    }
    if (bufferCreateInfo.size == 0) {
      return std::unexpected("VulkanDevice::createBufferVMA: Buffer size cannot be zero.");
    }

    vk::BufferCreateInfo c_bufferCreateInfo = bufferCreateInfo;
    vma::AllocationCreateInfo c_allocationCreateInfo = allocationCreateInfo;

    vk::Buffer outBuffer;
    vma::Allocation outAllocation;
    vma::AllocationInfo outAllocInfo;

    vk::Result result = vmaAllocator_.createBuffer(&c_bufferCreateInfo, &c_allocationCreateInfo,
                                                   &outBuffer, &outAllocation, &outAllocInfo);

    if (result != vk::Result::eSuccess) {
      return std::unexpected("VMA failed to create buffer: " + vk::to_string(result));
    }

    return VmaBuffer(vmaAllocator_, outBuffer, outAllocation, outAllocInfo, bufferCreateInfo.size);
  }

  [[nodiscard]] std::expected<VmaImage, std::string>
  createImageVMA(const vk::ImageCreateInfo &imageCreateInfo,
                 const vma::AllocationCreateInfo &allocationCreateInfo) NOEXCEPT {
    if (!vmaAllocator_) {
      return std::unexpected("VMA Allocator not initialized in VulkanDevice::createImageVMA.");
    }

    vk::ImageCreateInfo c_imageCreateInfo = imageCreateInfo;
    vma::AllocationCreateInfo c_allocationCreateInfo = allocationCreateInfo;

    vk::Image outImage;
    vma::Allocation outAllocation;
    vma::AllocationInfo outAllocInfo;

    vk::Result result = vmaAllocator_.createImage(&c_imageCreateInfo, &c_allocationCreateInfo,
                                                  &outImage, &outAllocation, &outAllocInfo);

    if (result != vk::Result::eSuccess) {
      return std::unexpected("VMA failed to create image: " + vk::to_string(result));
    }

    return VmaImage(vmaAllocator_, outImage, outAllocation, outAllocInfo, imageCreateInfo.format,
                    imageCreateInfo.extent);
  }

  [[nodiscard]] std::expected<vk::raii::CommandBuffer, std::string> beginSingleTimeCommands() {
    if (!*transientCommandPool_) {
      return std::unexpected("VulkanDevice: Transient command pool not initialized.");
    }

    vk::CommandBufferAllocateInfo allocInfo{.commandPool = *transientCommandPool_,
                                            .level = vk::CommandBufferLevel::ePrimary,
                                            .commandBufferCount = 1};

    auto cmdBuffersResult = device_.allocateCommandBuffers(allocInfo);
    if (!cmdBuffersResult) {
      return std::unexpected("Failed to allocate single-time command buffer: " +
                             vk::to_string(cmdBuffersResult.error()));
    }

    vk::raii::CommandBuffer commandBuffer = std::move(cmdBuffersResult.value().front());

    vk::CommandBufferBeginInfo beginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
    commandBuffer.begin(beginInfo);
    return std::move(commandBuffer);
  }

  [[nodiscard]] std::expected<void, std::string>
  endSingleTimeCommands(vk::raii::CommandBuffer commandBuffer) {
    if (!*commandBuffer || !*queue_ || !*device_) {
      return std::unexpected("VulkanDevice: Invalid parameter for endSingleTimeCommands.");
    }

    commandBuffer.end();

    vk::SubmitInfo submitInfo{.commandBufferCount = 1, .pCommandBuffers = &*commandBuffer};

    auto fenceResult = device_.createFence({});
    if (!fenceResult) {
      return std::unexpected("Failed to create fence for single-time command: " +
                             vk::to_string(fenceResult.error()));
    }
    vk::raii::Fence fence = std::move(*fenceResult);

    queue_.submit(submitInfo, *fence);

    vk::Result waitResult = device_.waitForFences({*fence}, VK_TRUE, UINT64_MAX);
    if (waitResult != vk::Result::eSuccess) {
      return std::unexpected("Failed to wait for single-time command fence: " +
                             vk::to_string(waitResult));
    }

    return {};
  }

  [[nodiscard]] std::expected<void, std::string>
  executeSingleTimeCommands(const std::function<void(vk::CommandBuffer)> &recordCommands) {
    vk::CommandBufferAllocateInfo allocInfo{.commandPool = *transientCommandPool_,
                                            .level = vk::CommandBufferLevel::ePrimary,
                                            .commandBufferCount = 1};

    auto cmdBuffersResult = device_.allocateCommandBuffers(allocInfo);
    if (!cmdBuffersResult) {
      return std::unexpected("Failed to allocate single-time command buffer: " +
                             vk::to_string(cmdBuffersResult.error()));
    }

    vk::raii::CommandBuffer commandBuffer = std::move(cmdBuffersResult.value().front());

    vk::CommandBufferBeginInfo beginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
    commandBuffer.begin(beginInfo);

    recordCommands(commandBuffer);

    commandBuffer.end();

    vk::SubmitInfo submitInfo{.commandBufferCount = 1, .pCommandBuffers = &*commandBuffer};

    queue_.submit(submitInfo, {});
    queue_.waitIdle();

    return {};
  }

  [[nodiscard]] std::expected<void, std::string>
  copyBuffer(vk::Buffer srcBuffer, vk::Buffer dstBuffer, vk::DeviceSize size) {
    return executeSingleTimeCommands([&](vk::CommandBuffer cmdBuffer) {
      vk::BufferCopy copyRegion{.srcOffset = 0, .dstOffset = 0, .size = size};
      cmdBuffer.copyBuffer(srcBuffer, dstBuffer, copyRegion);
    });
  }
};
