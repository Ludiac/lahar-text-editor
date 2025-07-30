module;

#include "macros.hpp"
#include "primitive_types.hpp"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

export module vulkan_app:ui;

import vulkan_hpp;
import std;

import :VulkanDevice;
import :VulkanPipeline;
import :VMA;
import :texture;

constexpr void convertToCCW(std::vector<uint32_t> &indices) {
  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    std::swap(indices[i + 1], indices[i + 2]);
  }
}

// The vertex layout for the single static quad.
export struct TextQuadVertex {
  glm::vec2 pos;
  glm::vec2 uv;
};

export struct TextInstanceData {
  glm::vec2 screenPos; // Top-left position of the quad
  glm::vec2 size;      // width and height of the quad
  glm::vec2 uvTopLeft;
  glm::vec2 uvBottomRight;
  glm::vec4 color; // NEW: Color is now per-instance
  glm::vec3 padding;
  float pxRange;
} __attribute__((aligned(64)));

// The vertex format for our static unit quad.
struct UIQuadVertex {
  glm::vec2 pos;
  glm::vec2 uv;
} __attribute__((aligned(16)));

export struct Quad {
  glm::vec2 position;
  vk::Extent2D size;
  glm::vec4 color;
  float zLayer;
};

struct UIInstanceData {
  Quad quad;
  float padding[3]; // Explicit padding to fill up to a 16-byte boundary or for future use
  // Future SDF parameters:
  float cornerRadius;
  float borderWidth;
  float padding2[2]; // Padding for the next float4
};

export struct RenderBatch {
  // A key for sorting. Lower numbers are drawn first.
  int sortKey{0};

  // Pipeline state
  vk::raii::Pipeline const *pipeline{};
  vk::raii::PipelineLayout const *pipelineLayout{};

  // Resources to bind (Descriptor Sets)
  vk::raii::DescriptorSet const *instanceDataSet{nullptr};
  vk::raii::DescriptorSet const *textureSet{nullptr};

  // Mesh data
  VmaBuffer const *vertexBuffer{};
  VmaBuffer const *indexBuffer{};
  uint32_t indexCount{};

  // Instancing data
  uint32_t instanceCount{};
  uint32_t firstInstance{};
  uint32_t dynamicOffset{0};

  // --- NEW: Generic Push Constant Data ---
  std::array<std::byte, 128> pushConstantData{}; // 128 bytes is the min guaranteed size
  uint32_t pushConstantSize = 0;
  vk::ShaderStageFlags pushConstantStages;

  // Comparison operator for sorting. We don't sort by push constants as they are expected to change
  // frequently.
  bool operator<(const RenderBatch &other) const {
    if (sortKey != other.sortKey) {
      return sortKey < other.sortKey;
    }
    if (pipeline != other.pipeline) {
      return pipeline < other.pipeline;
    }
    if (textureSet != other.textureSet) {
      return textureSet < other.textureSet;
    }
    // Add other criteria if needed
    return false;
  }
};

// The RenderQueue is simply a vector of batches for a given frame.
export using RenderQueue = std::vector<RenderBatch>;

void sortRenderQueue(std::vector<RenderBatch> &queue) {
  std::ranges::sort(queue, [](const RenderBatch &a, const RenderBatch &b) {
    if (a.sortKey != b.sortKey) {
      return a.sortKey < b.sortKey;
    }
    if (a.pipeline != b.pipeline) {
      return a.pipeline < b.pipeline;
    }
    if (a.textureSet != b.textureSet) {
      return a.textureSet < b.textureSet;
    }
    return false;
  });
}

export size_t padUniformBufferSize(size_t originalSize, size_t minAlignment) {
  if (minAlignment > 0) {
    return (originalSize + minAlignment - 1) & ~(minAlignment - 1);
  }
  return originalSize;
}

export [[nodiscard]] std::expected<void, std::string>
createStaticQuadBuffers(VulkanDevice &device, VmaBuffer &vertexBuffer, VmaBuffer &indexBuffer) {
  const std::vector<TextQuadVertex> VERTICES = {
      {.pos = {0.0, 1.0}, .uv = {0.0, 1.0}}, // Bottom-let
      {.pos = {1.0, 1.0}, .uv = {1.0, 1.0}}, // Bottom-right
      {.pos = {1.0, 0.0}, .uv = {1.0, 0.0}}, // Top-right
      {.pos = {0.0, 0.0}, .uv = {0.0, 0.0}}  // Top-let
  };
  std::vector<uint32_t> indices = {0, 1, 2, 2, 3, 0};
  convertToCCW(indices);

  auto createStagingBuffer = [&](vk::DeviceSize size,
                                 const void *data) -> std::expected<VmaBuffer, std::string> {
    vk::BufferCreateInfo bufInfo{.size = size, .usage = vk::BufferUsageFlagBits::eTransferSrc};
    vma::AllocationCreateInfo allocInfo{.flags = vma::AllocationCreateFlagBits::eMapped,
                                        .usage = vma::MemoryUsage::eCpuOnly};
    auto res = device.createBufferVMA(bufInfo, allocInfo);
    if (!res) {
      return std::unexpected("Failed to create staging buffer");
    }
    VmaBuffer buf = std::move(*res);
    std::memcpy(buf.getMappedData(), data, static_cast<size_t>(size));
    return std::expected<VmaBuffer, std::string>(std::move(buf));
  };

  auto createDeviceBuffer =
      [&](vk::DeviceSize size,
          vk::BufferUsageFlags usage) -> std::expected<VmaBuffer, std::string> {
    vk::BufferCreateInfo bufInfo{.size = size,
                                 .usage = usage | vk::BufferUsageFlagBits::eTransferDst};
    vma::AllocationCreateInfo allocInfo{.usage = vma::MemoryUsage::eGpuOnly};
    auto res = device.createBufferVMA(bufInfo, allocInfo);
    if (!res) {
      return std::unexpected("Failed to create device-local buffer");
    }
    return std::expected<VmaBuffer, std::string>(std::move(*res));
  };

  // Vertex buffer
  const vk::DeviceSize VERTEX_SIZE = sizeof(TextQuadVertex) * VERTICES.size();
  auto stagingVb = createStagingBuffer(VERTEX_SIZE, VERTICES.data());
  if (!stagingVb) {
    return std::unexpected("Failed to create text staging VB");
  }

  auto vertexBuf = createDeviceBuffer(VERTEX_SIZE, vk::BufferUsageFlagBits::eVertexBuffer);
  if (!vertexBuf) {
    return std::unexpected("Failed to create text static VB");
  }

  EXPECTED_VOID(device.copyBuffer(stagingVb->get(), vertexBuf->get(), VERTEX_SIZE));
  vertexBuffer = std::move(*vertexBuf);

  // Index buffer
  const vk::DeviceSize INDEX_SIZE = sizeof(uint32_t) * indices.size();
  auto stagingIb = createStagingBuffer(INDEX_SIZE, indices.data());
  if (!stagingIb) {
    return std::unexpected("Failed to create text staging IB");
  }

  auto indexBuf = createDeviceBuffer(INDEX_SIZE, vk::BufferUsageFlagBits::eIndexBuffer);
  if (!indexBuf) {
    return std::unexpected("Failed to create text static IB");
  }

  EXPECTED_VOID(device.copyBuffer(stagingIb->get(), indexBuf->get(), INDEX_SIZE));
  indexBuffer = std::move(*indexBuf);

  return {};
}

export [[nodiscard]] std::expected<void, std::string>
createInstanceBuffers(VulkanDevice &device, u32 frameCount, u32 size,
                      std::vector<VmaBuffer> &buffers, size_t instanceSize) {
  buffers.resize(frameCount);
  vk::DeviceSize bufferSize = size * instanceSize;

  for (u32 i = 0; i < frameCount; ++i) {
    vk::BufferCreateInfo bufferInfo{
        .size = bufferSize,
        .usage = vk::BufferUsageFlagBits::eStorageBuffer // It's a storage buffer now
    };
    // These buffers need to be updated from the CPU every frame.
    vma::AllocationCreateInfo allocInfo{
        .flags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite |
                 vma::AllocationCreateFlagBits::eMapped,
        .usage = vma::MemoryUsage::eAuto};

    auto bufferResult = device.createBufferVMA(bufferInfo, allocInfo);
    if (!bufferResult) {
      return std::unexpected("Failed to create text instance buffer " + std::to_string(i));
    }
    buffers[i] = std::move(*bufferResult);
  }
  return {};
}

// This is the core execution unit. It is stateless and simply processes
// the command list given to it.
export void processRenderQueue(const vk::raii::CommandBuffer &cmd, RenderQueue &queue) {
  if (queue.empty()) {
    return;
  }

  sortRenderQueue(queue);

  vk::raii::Pipeline const *lastPipeline = nullptr;
  vk::raii::DescriptorSet const *lastTextureSet = nullptr;
  VmaBuffer const *lastVertexBuffer = nullptr;
  VmaBuffer const *lastIndexBuffer = nullptr;

  for (const auto &batch : queue) {
    // 1. Bind Pipeline (only if it has changed)
    if (batch.pipeline != lastPipeline) {
      cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *batch.pipeline);
      lastPipeline = batch.pipeline;

      // A new pipeline means we MUST rebind all descriptors and buffers.
      lastTextureSet = nullptr;
      lastVertexBuffer = nullptr;
      lastIndexBuffer = nullptr;
    }

    // 2. Push Constants (ALWAYS, for every batch)
    // This is cheap and ensures every draw has the correct data.
    if (batch.pushConstantSize > 0) {
      cmd.pushConstants<std::array<std::byte, 128>>(*batch.pipelineLayout, batch.pushConstantStages,
                                                    0, // offset
                                                    batch.pushConstantData);
    }

    // 3. Bind Descriptor Sets
    // Instance data is expected to be unique per-batch, so we always bind it.
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *batch.pipelineLayout, 0,
                           {*batch.instanceDataSet}, {batch.dynamicOffset});

    if ((batch.textureSet != nullptr) && batch.textureSet != lastTextureSet) {
      cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *batch.pipelineLayout, 1,
                             {*batch.textureSet}, {});
      lastTextureSet = batch.textureSet;
    }

    // 4. Bind Buffers (only if they have changed)
    if (batch.vertexBuffer != lastVertexBuffer) {
      cmd.bindVertexBuffers(0, {batch.vertexBuffer->get()}, {0});
      lastVertexBuffer = batch.vertexBuffer;
    }
    if (batch.indexBuffer != lastIndexBuffer) {
      cmd.bindIndexBuffer(batch.indexBuffer->get(), 0, vk::IndexType::eUint32);
      lastIndexBuffer = batch.indexBuffer;
    }

    // 5. Draw!
    cmd.drawIndexed(batch.indexCount, batch.instanceCount, 0, 0, batch.firstInstance);
  }
}
