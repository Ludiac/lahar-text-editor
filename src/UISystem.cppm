module;

#include "macros.hpp"
#include "primitive_types.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

export module vulkan_app:UISystem;

import vulkan_hpp;
import std;

import :VulkanDevice;
import :VulkanPipeline;
import :VMA;
import :ui; // For Sheet, UIInstanceData, etc.

export struct UIPushConstants2D {
  glm::mat4 projection;
};

// This class is responsible for managing and preparing UI geometry for rendering.
// It no longer issues draw calls itself.
export class UISystem {
private:
  VulkanDevice *device{};
  u32 frameCount{0};
  u32 maxQuadsPerFrame{2048};
  std::vector<VmaBuffer> instanceBuffers;
  std::vector<UIInstanceData> queuedInstances; // All UI instances for the current frame
  VmaBuffer staticVertexBuffer;
  VmaBuffer staticIndexBuffer;
  vk::raii::DescriptorSetLayout instanceDataLayout{nullptr};
  std::vector<vk::raii::DescriptorSet> instanceDataDescriptorSets;

public:
  // Constructor now takes a pointer, or converts the reference to a pointer
  UISystem(VulkanDevice &dev, u32 inFlightFrameCount)
      : device(&dev), frameCount(inFlightFrameCount) {}
  UISystem() = default;

  // Private initialization function to handle fallible setup steps.
  [[nodiscard]] std::expected<void, std::string> initialize(const vk::raii::DescriptorPool &pool) {
    // Note: These helper functions are assumed to also return std::expected
    // Pass dereferenced device for helper functions that expect a reference
    if (auto res = createInstanceBuffers(*device, frameCount, maxQuadsPerFrame, instanceBuffers,
                                         sizeof(UIInstanceData));
        !res) {
      return std::unexpected("Failed to create UI instance buffers: " + res.error());
    }
    if (auto res = createStaticQuadBuffers(*device, staticVertexBuffer, staticIndexBuffer); !res) {
      return std::unexpected("Failed to create static quad buffers: " + res.error());
    }
    if (auto res = createInstanceDataDescriptorSetLayout(); !res) {
      return std::unexpected(res.error());
    }
    if (auto res = allocateInstanceDataDescriptorSets(pool); !res) {
      return std::unexpected(res.error());
    }
    return {};
  }

  // Factory function for safe, two-phase initialization.
  // Returns a fully initialized object by value or an error.
  [[nodiscard]] static std::expected<UISystem, std::string>
  create(VulkanDevice &dev, u32 inFlightFrameCount, const vk::raii::DescriptorPool &pool) {
    // Pass the reference to the constructor, which will convert it to a pointer
    UISystem system(dev, inFlightFrameCount);
    if (auto res = system.initialize(pool); !res) {
      return std::unexpected(res.error());
    }
    return std::move(system);
  }

  // Ensure the class is non-copyable but movable, as it manages GPU resources.
  UISystem(const UISystem &) = delete;
  UISystem &operator=(const UISystem &) = delete;
  UISystem(UISystem &&) = default;
  UISystem &operator=(UISystem &&) = default;

  ~UISystem() = default;

  void beginFrame() { queuedInstances.clear(); }

  void queueQuad(Quad quad) {
    if (queuedInstances.size() >= maxQuadsPerFrame) {
      return;
    }
    queuedInstances.emplace_back(UIInstanceData{.quad = quad});
  }

  // REPLACES the old `draw` method.
  // This method prepares the data and adds a RenderBatch to the queue.
  void prepareBatches(RenderQueue &queue, const VulkanPipeline &uiPipeline, u32 frameIndex,
                      vk::Extent2D windowSize) {
    glm::mat4 ortho = glm::ortho(0.0F, (float)windowSize.width, 0.0F, (float)windowSize.height);

    if (frameIndex >= frameCount || queuedInstances.empty()) {
      return;
    }

    // 1. Copy instance data to the GPU buffer for this frame
    VmaBuffer &currentInstanceBuffer = instanceBuffers[frameIndex];
    size_t dataSize = queuedInstances.size() * sizeof(UIInstanceData);
    // Ensure the buffer is large enough for all queued instances
    if (dataSize > currentInstanceBuffer.getAllocationInfo().size) {
      // Log an error or resize the buffer (more complex)
      return;
    }
    std::memcpy(static_cast<char *>(currentInstanceBuffer.getMappedData()), queuedInstances.data(),
                dataSize);
    RenderBatch batch{
        .sortKey = 100, // UI background elements could have a low sort key
        .pipeline = &uiPipeline.pipeline,
        .pipelineLayout = &uiPipeline.pipelineLayout,
        .instanceDataSet = &instanceDataDescriptorSets[frameIndex],
        .textureSet = nullptr, // No texture for basic UI sheets
        .vertexBuffer = &staticVertexBuffer,
        .indexBuffer = &staticIndexBuffer,
        .indexCount = 6, // Quad has 6 indices
        .instanceCount = static_cast<u32>(queuedInstances.size()),
        .firstInstance = 0,
        .dynamicOffset = 0,
        // .scale = {0, 0},
    };

    UIPushConstants2D pushConstants{
        .projection = ortho,
    };
    batch.pushConstantSize = sizeof(UIPushConstants2D);
    batch.pushConstantStages = vk::ShaderStageFlagBits::eVertex;
    std::memcpy(batch.pushConstantData.data(), &pushConstants, sizeof(UIPushConstants2D));

    queue.emplace_back(batch);

    // Update the descriptor set to reflect the *actual* number of instances we're drawing this
    // frame.
    updateDescriptorSets(static_cast<u32>(queuedInstances.size()), frameIndex);
  }

private:
  [[nodiscard]] std::expected<void, std::string> createInstanceDataDescriptorSetLayout() {
    vk::DescriptorSetLayoutBinding instanceBinding{.binding = 0,
                                                   .descriptorType =
                                                       vk::DescriptorType::eStorageBufferDynamic,
                                                   .descriptorCount = 1,
                                                   .stageFlags = vk::ShaderStageFlagBits::eVertex};
    vk::DescriptorSetLayoutCreateInfo layoutInfo{.bindingCount = 1, .pBindings = &instanceBinding};
    // Use -> to access members of the pointer
    auto layoutResult = device->logical().createDescriptorSetLayout(layoutInfo);
    if (!layoutResult) {
      return std::unexpected("Failed to create UI instance data descriptor set layout.");
    }
    instanceDataLayout = std::move(layoutResult.value());
    return {};
  }

  [[nodiscard]] std::expected<void, std::string>
  allocateInstanceDataDescriptorSets(const vk::raii::DescriptorPool &pool) {
    std::vector<vk::DescriptorSetLayout> layouts(frameCount, *instanceDataLayout);
    vk::DescriptorSetAllocateInfo allocInfo{
        .descriptorPool = pool, .descriptorSetCount = frameCount, .pSetLayouts = layouts.data()};
    // Use -> to access members of the pointer
    auto setsResult = device->logical().allocateDescriptorSets(allocInfo);
    if (!setsResult) {
      return std::unexpected("Failed to allocate UI instance descriptor sets.");
    }
    instanceDataDescriptorSets = std::move(setsResult.value());
    return {};
  }

  void updateDescriptorSets(u32 instanceNumber, u32 frameIndex) {
    vk::DescriptorBufferInfo bufferInfo{
        .buffer = instanceBuffers[frameIndex].get(),
        .offset = 0,
        .range = instanceNumber * sizeof(UIInstanceData), // <--- CHANGE to UIInstanceData
    };
    vk::WriteDescriptorSet instanceWrite{.dstSet = instanceDataDescriptorSets[frameIndex],
                                         .dstBinding = 0,
                                         .descriptorCount = 1,
                                         .descriptorType =
                                             vk::DescriptorType::eStorageBufferDynamic,
                                         .pBufferInfo = &bufferInfo};
    // Use -> to access members of the pointer
    device->logical().updateDescriptorSets({instanceWrite}, nullptr);
  }
};
