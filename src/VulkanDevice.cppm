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

namespace {
static u32 selectQueueFamilyIndex(const vk::raii::PhysicalDevice &physicalDevice) {
  auto queueFamilyProperties = physicalDevice.getQueueFamilyProperties();

  for (u32 i = 0; i < queueFamilyProperties.size(); i++) {
    if (queueFamilyProperties[i].queueFlags & vk::QueueFlagBits::eGraphics) {
      return i;
    }
  }

  return (u32)-1;
}

class VulkanQueue {
public:
  VulkanQueue() = default;

  std::expected<void, std::string> create(const vk::raii::Device &device, uint32_t familyIndex,
                                          uint32_t queueIndex = 0) {
    ENSURE_INIT(*device);

    if (auto expected = device.getQueue(familyIndex, 0); expected) {
      m_queue = std::move(*expected);
    } else {
      return std::unexpected(std::format("error with queue {}", vk::to_string(expected.error())));
    }

    m_familyIndex = familyIndex;

    return {};
  }

  auto operator->() { return &m_queue; }
  auto operator->() const { return &m_queue; }
  operator vk::raii::Queue &() { return m_queue; }
  operator const vk::raii::Queue &() const { return m_queue; }
  const vk::Queue &operator*() const noexcept { return *m_queue; }

  [[nodiscard]] u32 queueFamily() const { return m_familyIndex; }

private:
  vk::raii::Queue m_queue{nullptr};
  u32 m_familyIndex{static_cast<uint32_t>(-1)};
};
} // namespace

export struct BufferResources {
  vk::raii::Buffer buffer{nullptr};
  vk::raii::DeviceMemory memory{nullptr};
};

export class VulkanDevice {
  vk::raii::PhysicalDevice m_physicalDevice{nullptr};
  vk::raii::Device m_device{nullptr};
  vk::raii::DescriptorPool m_descriptorPool{nullptr};
  vma::Allocator m_vmaAllocator;
  vk::raii::CommandPool m_transientCommandPool{nullptr};

  vk::PhysicalDeviceLimits m_limits;
  VulkanQueue m_gfxQueue{};

  [[nodiscard]] std::expected<void, std::string>
  pickPhysicalDevice(const vk::raii::Instance &instance) {
    auto expected = instance.enumeratePhysicalDevices();
    if (expected) {
      if (expected->empty()) {
        return std::unexpected("No Vulkan-compatible physical devices found!");
      }
      m_physicalDevice = std::move(expected->front());
      m_limits = m_physicalDevice.getProperties().limits;
      return {};
    }
    return std::unexpected("Failed to enumerate physical devices: " +
                           vk::to_string(expected.error()));
  }

  [[nodiscard]] std::expected<void, std::string>
  createLogicalDevice(const vk::raii::Instance &instance) {
    auto queueFamily = selectQueueFamilyIndex(m_physicalDevice);
    if (std::cmp_equal(queueFamily, -1)) {
      return std::unexpected("Failed to select queue family index");
    }

    std::vector<const char *> deviceExtensions;
    deviceExtensions.push_back(vk::KHRSwapchainExtensionName);
    deviceExtensions.push_back(vk::KHRMapMemory2ExtensionName);

    u32 propertiesCount = 0;
    std::vector<vk::ExtensionProperties> properties =
        m_physicalDevice.enumerateDeviceExtensionProperties();

    const float QUEUE_PRIORITY[] = {1.0};
    vk::DeviceQueueCreateInfo queueInfo[1] = {};
    queueInfo[0].queueFamilyIndex = queueFamily;
    queueInfo[0].queueCount = 1;
    queueInfo[0].pQueuePriorities = QUEUE_PRIORITY;

    vk::PhysicalDeviceFeatures enabledFeatures{};
    vk::PhysicalDeviceVulkan11Features enabledFeatures11{
        .shaderDrawParameters = vk::True,
    };
    enabledFeatures.samplerAnisotropy = vk::True;
    if (auto expected = m_physicalDevice.createDevice({
            .pNext = &enabledFeatures11,
            .queueCreateInfoCount = sizeof(queueInfo) / sizeof(queueInfo[0]),
            .pQueueCreateInfos = queueInfo,
            .enabledExtensionCount = static_cast<u32>(deviceExtensions.size()),
            .ppEnabledExtensionNames = deviceExtensions.data(),
            .pEnabledFeatures = &enabledFeatures,
        });
        expected) {
      m_device = std::move(*expected);
    } else {
      return std::unexpected(std::format("error with device {}", vk::to_string(expected.error())));
    }
    create if (auto expected = m_gfxQueue.init(m_device, queueFamily, 0); !expected) {
      return expected;
    }

    vk::CommandPoolCreateInfo poolCreateInfo{.flags =
                                                 vk::CommandPoolCreateFlagBits::eTransient |
                                                 vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                             .queueFamilyIndex = queueFamily};
    auto transientPoolResult = m_device.createCommandPool(poolCreateInfo);
    if (!transientPoolResult) {
      return std::unexpected("Failed to create transient command pool: " +
                             vk::to_string(transientPoolResult.error()));
    }
    m_transientCommandPool = std::move(transientPoolResult.value());

    if (auto expected = createVmaAllocator(instance, m_physicalDevice, m_device); expected) {
      m_vmaAllocator = *expected;
      return {};
    }
    return std::unexpected(std::format("Failed to create Vma allocator:"));
  }

public:
  ~VulkanDevice() { m_vmaAllocator.destroy(); }

  [[nodiscard]] const vk::raii::Device &logical() const { return m_device; }
  [[nodiscard]] const vk::raii::PhysicalDevice &physical() const { return m_physicalDevice; }
  [[nodiscard]] const vma::Allocator &allocator() const { return m_vmaAllocator; }
  [[nodiscard]] const VulkanQueue &graphicsQueue() const noexcept { return m_gfxQueue; }
  [[nodiscard]] const vk::PhysicalDeviceLimits &limits() const noexcept { return m_limits; }
  [[nodiscard]] const vk::raii::DescriptorPool &descriptorPool() const noexcept {
    return m_descriptorPool;
  }

  ImGui_ImplVulkan_InitInfo initInfo() {
    return ImGui_ImplVulkan_InitInfo{
        .PhysicalDevice = *m_physicalDevice,
        .Device = *m_device,
        .QueueFamily = m_gfxQueue.queueFamily(),
        .Queue = *m_gfxQueue,
        .DescriptorPool = *m_descriptorPool,
    };
  }

  [[nodiscard]] std::expected<void, std::string> create(const vk::raii::Instance &instance) {
    ENSURE_INIT(*instance);

    if (auto result = pickPhysicalDevice(instance); !result) {
      return result;
    }

    if (auto result = createLogicalDevice(instance); !result) {
      return result;
    }

    // ideally createDescriptorPool() would be called here but since its impossible to know
    // imageCount before window creation which itself requires valid vk handles we call it
    // separately

    return {};
  }

  [[nodiscard]] std::expected<void, std::string> createDescriptorPool(u32 imageCountBasedFactor) {
    if (!*m_device) {
      return std::unexpected("VulkanDevice::createDescriptorPool: Logical device is null. Call "
                             "createLogicalDevice first.");
    }

    u32 appUniformBuffers = imageCountBasedFactor;
    u32 appDynamicUniformBuffers = imageCountBasedFactor;
    u32 appCombinedImageSamplers = imageCountBasedFactor;

    std::vector<vk::DescriptorPoolSize> pool_sizes = {
        {.type = vk::DescriptorType::eUniformBuffer, .descriptorCount = appUniformBuffers},
        {.type = vk::DescriptorType::eUniformBufferDynamic,
         .descriptorCount = appDynamicUniformBuffers},
        {.type = vk::DescriptorType::eCombinedImageSampler,
         .descriptorCount =
             appCombinedImageSamplers + IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE},
        {.type = vk::DescriptorType::eStorageBuffer, .descriptorCount = 30},
        {.type = vk::DescriptorType::eStorageBufferDynamic, .descriptorCount = 10},
    };

    u32 applicationMaxSets = imageCountBasedFactor * 2;

    u32 imguiEstimatedSets = 10;

    vk::DescriptorPoolCreateInfo poolInfo{.flags =
                                              vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
                                          .maxSets = applicationMaxSets + imguiEstimatedSets,
                                          .poolSizeCount = static_cast<u32>(pool_sizes.size()),
                                          .pPoolSizes = pool_sizes.data()};

    if (*m_descriptorPool) {
      // Consider device.waitIdle() here if descriptor sets from the old pool might still be in use.
      // For simplicity, assuming this is called during setup or a controlled resize.
      m_descriptorPool.clear();
    }

    auto poolResult = m_device.createDescriptorPool(poolInfo);
    if (!poolResult) {
      return std::unexpected(
          std::format("VulkanDevice::createDescriptorPool: Failed to create descriptor pool: {} - "
                      "MaxSets: {}, PoolSizes: UBOs({}), DynUBOs({}), Samplers({})",
                      vk::to_string(poolResult.error()), poolInfo.maxSets,
                      (!pool_sizes.empty() ? pool_sizes[0].descriptorCount : 0),
                      (pool_sizes.size() > 1 ? pool_sizes[1].descriptorCount : 0),
                      (pool_sizes.size() > 2 ? pool_sizes[2].descriptorCount : 0)));
    }
    m_descriptorPool = std::move(poolResult.value());
    return {};
  }

  [[nodiscard]] std::expected<BufferResources, std::string>
  createBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage,
               vk::MemoryPropertyFlags memoryProperties) NOEXCEPT {
    BufferResources resources;

    if (auto buf = m_device.createBuffer(
            {.size = size, .usage = usage, .sharingMode = vk::SharingMode::eExclusive})) {
      resources.buffer = std::move(*buf);
    } else {
      return std::unexpected("Buffer creation failed: " + vk::to_string(buf.error()));
    }

    auto memReqs = m_device.getBufferMemoryRequirements2({.buffer = resources.buffer});
    u32 memType = 0;
    if (auto memTypeExp = findMemoryType(
            m_physicalDevice, memReqs.memoryRequirements.memoryTypeBits, memoryProperties)) {
      memType = *memTypeExp;
    } else {
      return std::unexpected(memTypeExp.error());
    }

    if (auto mem = m_device.allocateMemory(
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
    if (!m_vmaAllocator) {
      return std::unexpected("VMA Allocator not initialized in VulkanDevice::createBufferVMA.");
    }
    if (bufferCreateInfo.size == 0) {
      return std::unexpected("VulkanDevice::createBufferVMA: Buffer size cannot be zero.");
    }

    vk::BufferCreateInfo cBufferCreateInfo = bufferCreateInfo;
    vma::AllocationCreateInfo cAllocationCreateInfo = allocationCreateInfo;

    vk::Buffer outBuffer;
    vma::Allocation outAllocation;
    vma::AllocationInfo outAllocInfo;

    vk::Result result = m_vmaAllocator.createBuffer(&cBufferCreateInfo, &cAllocationCreateInfo,
                                                    &outBuffer, &outAllocation, &outAllocInfo);

    if (result != vk::Result::eSuccess) {
      return std::unexpected("VMA failed to create buffer: " + vk::to_string(result));
    }

    return VmaBuffer(m_vmaAllocator, outBuffer, outAllocation, outAllocInfo, bufferCreateInfo.size);
  }

  [[nodiscard]] std::expected<VmaImage, std::string>
  createImageVMA(const vk::ImageCreateInfo &imageCreateInfo,
                 const vma::AllocationCreateInfo &allocationCreateInfo) NOEXCEPT {
    if (!m_vmaAllocator) {
      return std::unexpected("VMA Allocator not initialized in VulkanDevice::createImageVMA.");
    }

    vk::ImageCreateInfo cImageCreateInfo = imageCreateInfo;
    vma::AllocationCreateInfo cAllocationCreateInfo = allocationCreateInfo;

    vk::Image outImage;
    vma::Allocation outAllocation;
    vma::AllocationInfo outAllocInfo;

    vk::Result result = m_vmaAllocator.createImage(&cImageCreateInfo, &cAllocationCreateInfo,
                                                   &outImage, &outAllocation, &outAllocInfo);

    if (result != vk::Result::eSuccess) {
      return std::unexpected("VMA failed to create image: " + vk::to_string(result));
    }

    return VmaImage(m_vmaAllocator, outImage, outAllocation, outAllocInfo, imageCreateInfo.format,
                    imageCreateInfo.extent);
  }

  [[nodiscard]] std::expected<vk::raii::CommandBuffer, std::string> beginSingleTimeCommands() {
    if (!*m_transientCommandPool) {
      return std::unexpected("VulkanDevice: Transient command pool not initialized.");
    }

    vk::CommandBufferAllocateInfo allocInfo{.commandPool = *m_transientCommandPool,
                                            .level = vk::CommandBufferLevel::ePrimary,
                                            .commandBufferCount = 1};

    auto cmdBuffersResult = m_device.allocateCommandBuffers(allocInfo);
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
    if (!*commandBuffer || !*m_gfxQueue || !*m_device) {
      return std::unexpected("VulkanDevice: Invalid parameter for endSingleTimeCommands.");
    }

    commandBuffer.end();

    vk::SubmitInfo submitInfo{.commandBufferCount = 1, .pCommandBuffers = &*commandBuffer};

    auto fenceResult = m_device.createFence({});
    if (!fenceResult) {
      return std::unexpected("Failed to create fence for single-time command: " +
                             vk::to_string(fenceResult.error()));
    }
    vk::raii::Fence fence = std::move(*fenceResult);

    m_gfxQueue->submit(submitInfo, *fence);

    vk::Result waitResult = m_device.waitForFences({*fence}, VK_TRUE, UINT64_MAX);
    if (waitResult != vk::Result::eSuccess) {
      return std::unexpected("Failed to wait for single-time command fence: " +
                             vk::to_string(waitResult));
    }

    return {};
  }

  [[nodiscard]] std::expected<void, std::string>
  executeSingleTimeCommands(const std::function<void(vk::CommandBuffer)> &recordCommands) {
    vk::CommandBufferAllocateInfo allocInfo{.commandPool = *m_transientCommandPool,
                                            .level = vk::CommandBufferLevel::ePrimary,
                                            .commandBufferCount = 1};

    auto cmdBuffersResult = m_device.allocateCommandBuffers(allocInfo);
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

    m_gfxQueue->submit(submitInfo, {});
    m_gfxQueue->waitIdle();

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
