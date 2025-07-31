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
  VulkanDevice *m_device{};
  u32 m_frameCount{0};
  u32 m_maxQuadsPerFrame{2048};
  std::vector<VmaBuffer> m_instanceBuffers;
  std::vector<UIInstanceData> m_queuedInstances; // All UI instances for the current frame
  VmaBuffer m_staticVertexBuffer;
  VmaBuffer m_staticIndexBuffer;
  vk::raii::DescriptorSetLayout m_instanceDataLayout{nullptr};
  std::vector<vk::raii::DescriptorSet> m_instanceDataDescriptorSets;

  // Private initialization function to handle fallible setup steps.
  [[nodiscard]] std::expected<void, std::string> initialize(const vk::raii::DescriptorPool &pool) {
    // Note: These helper functions are assumed to also return std::expected
    // Pass dereferenced device for helper functions that expect a reference
    if (auto res = createInstanceBuffers(*m_device, m_frameCount, m_maxQuadsPerFrame,
                                         m_instanceBuffers, sizeof(UIInstanceData));
        !res) {
      return std::unexpected("Failed to create UI instance buffers: " + res.error());
    }
    if (auto res = createStaticQuadBuffers(*m_device, m_staticVertexBuffer, m_staticIndexBuffer);
        !res) {
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

  // Constructor now takes a pointer, or converts the reference to a pointer
  UISystem(VulkanDevice &dev, u32 inFlightFrameCount)
      : m_device(&dev), m_frameCount(inFlightFrameCount) {}

public:
  UISystem() = default;

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

  void beginFrame() { m_queuedInstances.clear(); }

  void queueQuad(Quad quad) {
    if (m_queuedInstances.size() >= m_maxQuadsPerFrame) {
      return;
    }
    m_queuedInstances.emplace_back(UIInstanceData{.screenPos = quad.position,
                                                  .size = quad.size,
                                                  .color = quad.color,
                                                  .z_layer = quad.zLayer});
  }

  // REPLACES the old `draw` method.
  // This method prepares the data and adds a RenderBatch to the queue.
  void prepareBatches(RenderQueue &queue, const VulkanPipeline &uiPipeline, u32 frameIndex,
                      vk::Extent2D windowSize) {
    ENSURE_INIT(m_device);

    glm::mat4 ortho = glm::ortho(0.0F, (float)windowSize.width, 0.0F, (float)windowSize.height);

    if (frameIndex >= m_frameCount || m_queuedInstances.empty()) {
      return;
    }

    // 1. Copy instance data to the GPU buffer for this frame
    VmaBuffer &currentInstanceBuffer = m_instanceBuffers[frameIndex];
    size_t dataSize = m_queuedInstances.size() * sizeof(UIInstanceData);
    // Ensure the buffer is large enough for all queued instances
    if (dataSize > currentInstanceBuffer.getAllocationInfo().size) {
      // Log an error or resize the buffer (more complex)
      return;
    }
    std::memcpy(static_cast<char *>(currentInstanceBuffer.getMappedData()),
                m_queuedInstances.data(), dataSize);
    RenderBatch batch{
        .sortKey = 100,
        .pipeline = &uiPipeline.pipeline,
        .pipelineLayout = &uiPipeline.pipelineLayout,
        .instanceDataSet = &m_instanceDataDescriptorSets[frameIndex],
        .textureSet = nullptr,
        .vertexBuffer = &m_staticVertexBuffer,
        .indexBuffer = &m_staticIndexBuffer,
        .indexCount = 6,
        .instanceCount = static_cast<u32>(m_queuedInstances.size()),
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
    updateDescriptorSets(static_cast<u32>(m_queuedInstances.size()), frameIndex);
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
    auto layoutResult = m_device->logical().createDescriptorSetLayout(layoutInfo);
    if (!layoutResult) {
      return std::unexpected("Failed to create UI instance data descriptor set layout.");
    }
    m_instanceDataLayout = std::move(layoutResult.value());
    return {};
  }

  [[nodiscard]] std::expected<void, std::string>
  allocateInstanceDataDescriptorSets(const vk::raii::DescriptorPool &pool) {
    std::vector<vk::DescriptorSetLayout> layouts(m_frameCount, *m_instanceDataLayout);
    vk::DescriptorSetAllocateInfo allocInfo{
        .descriptorPool = pool, .descriptorSetCount = m_frameCount, .pSetLayouts = layouts.data()};
    // Use -> to access members of the pointer
    auto setsResult = m_device->logical().allocateDescriptorSets(allocInfo);
    if (!setsResult) {
      return std::unexpected("Failed to allocate UI instance descriptor sets.");
    }
    m_instanceDataDescriptorSets = std::move(setsResult.value());
    return {};
  }

  void updateDescriptorSets(u32 instanceNumber, u32 frameIndex) {
    vk::DescriptorBufferInfo bufferInfo{
        .buffer = m_instanceBuffers[frameIndex].get(),
        .offset = 0,
        .range = instanceNumber * sizeof(UIInstanceData), // <--- CHANGE to UIInstanceData
    };
    vk::WriteDescriptorSet instanceWrite{.dstSet = m_instanceDataDescriptorSets[frameIndex],
                                         .dstBinding = 0,
                                         .descriptorCount = 1,
                                         .descriptorType =
                                             vk::DescriptorType::eStorageBufferDynamic,
                                         .pBufferInfo = &bufferInfo};
    // Use -> to access members of the pointer
    m_device->logical().updateDescriptorSets({instanceWrite}, nullptr);
  }
};
