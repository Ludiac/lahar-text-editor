module;

#include "macros.hpp"
#include "primitive_types.hpp"
#include <glm/glm.hpp>

export module vulkan_app:mesh;
import vulkan_hpp;
import std;
import :VulkanDevice;
import :texture;
import :VulkanPipeline;

export class Mesh {
public:
  std::string name;
  Material material;
  PBRTextures textures;
  u32 indexCount{0};

private:
  VulkanDevice &m_device;
  u32 m_imageCount_member;

  std::vector<Vertex> m_vertices_data;
  std::vector<u32> m_indices_data;

  VmaBuffer m_vertexBuffer;
  VmaBuffer m_indexBuffer;
  VmaBuffer m_mvpUniformBuffers;
  VmaBuffer m_materialUniformBuffer;

  std::vector<vk::raii::DescriptorSet> m_descriptorSets;

  [[nodiscard]] std::expected<void, std::string> createVertexBuffer() NOEXCEPT {
    if (m_vertices_data.empty()) {
      return {};
}
    const auto BUFFER_SIZE = sizeof(Vertex) * m_vertices_data.size();

    vk::BufferCreateInfo bufferInfo{.size = BUFFER_SIZE,
                                    .usage = vk::BufferUsageFlagBits::eVertexBuffer

    };
    vma::AllocationCreateInfo allocInfo{
        .flags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite |
                 vma::AllocationCreateFlagBits::eMapped,
        .usage = vma::MemoryUsage::eAutoPreferHost

    };

    auto bufferResult = m_device.createBufferVMA(bufferInfo, allocInfo);
    if (!bufferResult) {
      return std::unexpected("Mesh '" + name +
                             "': Vertex VmaBuffer creation: " + bufferResult.error());
}
    m_vertexBuffer = std::move(bufferResult.value());

    if (m_vertexBuffer.getMappedData() == nullptr) {
      return std::unexpected("Mesh '" + name + "': Vertex VmaBuffer not mapped after creation.");
}

    std::memcpy(m_vertexBuffer.getMappedData(), m_vertices_data.data(), BUFFER_SIZE);

    return {};
  }

  [[nodiscard]] std::expected<void, std::string> createIndexBuffer() NOEXCEPT {
    if (m_indices_data.empty()) {
      this->indexCount = 0;
      return {};
    }
    this->indexCount = static_cast<u32>(m_indices_data.size());
    const auto BUFFER_SIZE = sizeof(u32) * m_indices_data.size();

    vk::BufferCreateInfo bufferInfo{.size = BUFFER_SIZE,
                                    .usage = vk::BufferUsageFlagBits::eIndexBuffer};
    vma::AllocationCreateInfo allocInfo{
        .flags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite |
                 vma::AllocationCreateFlagBits::eMapped,
        .usage = vma::MemoryUsage::eAutoPreferHost};
    auto bufferResult = m_device.createBufferVMA(bufferInfo, allocInfo);
    if (!bufferResult) {
      return std::unexpected("Mesh '" + name +
                             "': Index VmaBuffer creation: " + bufferResult.error());
}
    m_indexBuffer = std::move(bufferResult.value());

    if (m_indexBuffer.getMappedData() == nullptr) {
      return std::unexpected("Mesh '" + name + "': Index VmaBuffer not mapped.");
}
    std::memcpy(m_indexBuffer.getMappedData(), m_indices_data.data(), BUFFER_SIZE);
    return {};
  }

  [[nodiscard]] std::expected<void, std::string> createMvpUniformBuffers() NOEXCEPT {
    if (m_imageCount_member == 0) {
      return {};
}
    auto bufferSize = sizeof(UniformBufferObject) * m_imageCount_member;
    if (bufferSize == 0) {
      return {};
}

    vk::BufferCreateInfo bufferInfo{.size = bufferSize,
                                    .usage = vk::BufferUsageFlagBits::eUniformBuffer};
    vma::AllocationCreateInfo allocInfo{
        .flags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite |
                 vma::AllocationCreateFlagBits::eMapped,
        .usage = vma::MemoryUsage::eAutoPreferHost};
    auto bufferResult = m_device.createBufferVMA(bufferInfo, allocInfo);
    if (!bufferResult) {
      return std::unexpected("Mesh '" + name +
                             "': MVP UBO VmaBuffer creation: " + bufferResult.error());
}
    m_mvpUniformBuffers = std::move(bufferResult.value());
    return {};
  }

  [[nodiscard]] std::expected<void, std::string> createSingleMaterialUniformBuffer() NOEXCEPT {
    if (m_imageCount_member == 0) {
      return {};
}
    auto alignment = m_device.physical().getProperties().limits.minUniformBufferOffsetAlignment;
    auto alignedMaterialSize = (sizeof(Material) + alignment - 1) & ~(alignment - 1);
    auto bufferSize = alignedMaterialSize * m_imageCount_member;
    if (bufferSize == 0) {
      return {};
}

    vk::BufferCreateInfo bufferInfo{.size = bufferSize,
                                    .usage = vk::BufferUsageFlagBits::eUniformBuffer};
    vma::AllocationCreateInfo allocInfo{
        .flags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite |
                 vma::AllocationCreateFlagBits::eMapped,
        .usage = vma::MemoryUsage::eAutoPreferHost};
    auto bufferResult = m_device.createBufferVMA(bufferInfo, allocInfo);
    if (!bufferResult) {
      return std::unexpected("Mesh '" + name +
                             "': Material UBO VmaBuffer creation: " + bufferResult.error());
}
    m_materialUniformBuffer = std::move(bufferResult.value());

    for (u32 i = 0; i < m_imageCount_member; ++i) {
      updateMaterialUniformBufferData(i);
    }
    return {};
  }

public:
  Mesh(VulkanDevice &dev, std::string meshName, std::vector<Vertex> &&verts,
       std::vector<u32> &&meshIndices, Material initMaterial, PBRTextures initPbrTextures,
       u32 numImages)
      : m_device(dev), name(std::move(meshName)), material(initMaterial),
        textures(std::move(initPbrTextures)), m_imageCount_member(numImages),
        m_vertices_data(std::move(verts)), m_indices_data(std::move(meshIndices)) {
    EXPECTED_VOID(createVertexBuffer());
    EXPECTED_VOID(createIndexBuffer());
    EXPECTED_VOID(createMvpUniformBuffers());
    EXPECTED_VOID(createSingleMaterialUniformBuffer());
  }

  void updateMaterialUniformBufferData(u32 currentImage) {
    if (!m_materialUniformBuffer || currentImage >= m_imageCount_member) {
      return;
}
    if (m_materialUniformBuffer.getMappedData() == nullptr) {
      std::println("Error: Mesh '{}': Material UBO not mapped for image index {}.", name,
                   currentImage);
      return;
    }
    auto alignment = m_device.physical().getProperties().limits.minUniformBufferOffsetAlignment;
    auto alignedMaterialSize = (sizeof(Material) + alignment - 1) & ~(alignment - 1);
    vk::DeviceSize offset = currentImage * alignedMaterialSize;
    char *baseMapped = static_cast<char *>(m_materialUniformBuffer.getMappedData());
    std::memcpy(baseMapped + offset, &this->material, sizeof(Material));
  }

  void updateMvpUniformBuffer(u32 currentImage, const glm::mat4 &model, const glm::mat4 &view,
                              const glm::mat4 &projection) {
    if (!m_mvpUniformBuffers || currentImage >= m_imageCount_member) {
      return;
}
    if (m_mvpUniformBuffers.getMappedData() == nullptr) {
      std::println("Error: Mesh '{}': MVP UBO not mapped for image index {}.", name, currentImage);
      return;
    }

    UniformBufferObject ubo{};
    ubo.model = model;
    ubo.view = view;
    ubo.projection = projection;
    ubo.inverseView = glm::inverse(view);
    ubo.normalMatrix = glm::transpose(glm::inverse(model));

    vk::DeviceSize offset = sizeof(UniformBufferObject) * currentImage;
    char *baseMapped = static_cast<char *>(m_mvpUniformBuffers.getMappedData());
    std::memcpy(baseMapped + offset, &ubo, sizeof(ubo));
  }

  [[nodiscard]] std::expected<void, std::string>
  allocateDescriptorSets(const vk::raii::DescriptorPool &pool,
                         const vk::raii::DescriptorSetLayout &combinedLayout) {
    if (m_imageCount_member == 0) {
      std::string errorMsg =
          "Mesh " + name + ": Image count is zero for descriptor set allocation.";
      return std::unexpected(errorMsg);
    }
    if (!*combinedLayout) {
      std::string errorMsg =
          "Mesh " + name + ": Combined layout is null for descriptor set allocation.";
      return std::unexpected(errorMsg);
    }
    std::vector<vk::DescriptorSetLayout> layouts(m_imageCount_member, *combinedLayout);
    vk::DescriptorSetAllocateInfo allocInfo{
        .descriptorPool = *pool,
        .descriptorSetCount = m_imageCount_member,
        .pSetLayouts = layouts.data(),
    };
    auto setsResult = m_device.logical().allocateDescriptorSets(allocInfo);
    if (!setsResult) {
      std::string errorMsg = "Mesh " + name + ": Failed to allocate combined descriptor sets: " +
                             vk::to_string(setsResult.error());
      return std::unexpected(errorMsg);
    }
    m_descriptorSets = std::move(setsResult.value());
    return {};
  }

  void updateDescriptorSetContents(u32 currentImage) {
    if (currentImage >= m_imageCount_member || m_descriptorSets.empty() ||
        currentImage >= m_descriptorSets.size() || !*m_descriptorSets[currentImage]) {
      return;
    }
    std::vector<vk::WriteDescriptorSet> writes;
    vk::DescriptorBufferInfo mvpUboInfoStorage;
    vk::DescriptorBufferInfo materialUboInfoStorage;
    vk::DescriptorImageInfo baseColorTextureInfoStorage;
    vk::DescriptorImageInfo normalTextureInfoStorage;
    vk::DescriptorImageInfo metallicRoughnessTextureInfoStorage;
    vk::DescriptorImageInfo occlusionTextureInfoStorage;
    vk::DescriptorImageInfo emissiveTextureInfoStorage;
    if (m_mvpUniformBuffers) {
      mvpUboInfoStorage =
          vk::DescriptorBufferInfo{.buffer = m_mvpUniformBuffers.buffer,
                                   .offset = currentImage * sizeof(UniformBufferObject),
                                   .range = sizeof(UniformBufferObject)};
      writes.emplace_back(
          vk::WriteDescriptorSet{.dstSet = *m_descriptorSets[currentImage],
                                 .dstBinding = 0,
                                 .dstArrayElement = 0,
                                 .descriptorCount = 1,
                                 .descriptorType = vk::DescriptorType::eUniformBuffer,
                                 .pBufferInfo = &mvpUboInfoStorage});
    } else {
    }

    if (m_materialUniformBuffer) {
      auto alignment = m_device.physical().getProperties().limits.minUniformBufferOffsetAlignment;
      auto alignedMaterialSize = (sizeof(Material) + alignment - 1) & ~(alignment - 1);
      materialUboInfoStorage =
          vk::DescriptorBufferInfo{.buffer = m_materialUniformBuffer.buffer,
                                   .offset = currentImage * alignedMaterialSize,
                                   .range = sizeof(Material)};
      writes.emplace_back(
          vk::WriteDescriptorSet{.dstSet = *m_descriptorSets[currentImage],
                                 .dstBinding = 1,
                                 .dstArrayElement = 0,
                                 .descriptorCount = 1,
                                 .descriptorType = vk::DescriptorType::eUniformBuffer,
                                 .pBufferInfo = &materialUboInfoStorage});
    } else {
    }

    auto createTextureWrite = [&](uint32_t binding, vk::DescriptorImageInfo &imageInfo,
                                    const std::shared_ptr<Texture> &texture) {
      if (!texture) {
        std::print("no texture {}", binding);
        return;
      }
      if (!*texture->view) {
        std::print(", no view {}", binding);
        return;
      }
      if (!*texture->sampler) {
        std::println(", no sampler {}", binding);
        return;
      }
      if (texture && *texture->view && *texture->sampler) {
        imageInfo = vk::DescriptorImageInfo{.sampler = *texture->sampler,
                                            .imageView = *texture->view,
                                            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
        writes.emplace_back(
            vk::WriteDescriptorSet{.dstSet = *m_descriptorSets[currentImage],
                                   .dstBinding = binding,
                                   .dstArrayElement = 0,
                                   .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                   .pImageInfo = &imageInfo});
      }
    };

    createTextureWrite(2, baseColorTextureInfoStorage, textures.baseColor);
    createTextureWrite(3, normalTextureInfoStorage, textures.normal);
    createTextureWrite(4, metallicRoughnessTextureInfoStorage, textures.metallicRoughness);
    createTextureWrite(5, occlusionTextureInfoStorage, textures.occlusion);
    createTextureWrite(6, emissiveTextureInfoStorage, textures.emissive);

    if (!writes.empty()) {
      m_device.logical().updateDescriptorSets(writes, nullptr);
    } else {
    }
  }

  void bind(vk::raii::CommandBuffer &cmd, VulkanPipeline *pipeline, u32 currentImage) const {
    if (!m_vertexBuffer) {
      return;
    }
    if (!m_indexBuffer && indexCount > 0) {
      return;
    }
    if (m_descriptorSets.empty() || currentImage >= m_descriptorSets.size() ||
        !*m_descriptorSets[currentImage]) {
      return;
    }
    if ((pipeline == nullptr) || !*pipeline->pipelineLayout) {
      return;
    }
    cmd.bindVertexBuffers(0, {m_vertexBuffer.buffer}, {0});
    if (indexCount > 0 && m_indexBuffer) {
      cmd.bindIndexBuffer(m_indexBuffer.buffer, 0, vk::IndexType::eUint32);
    }
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline->pipelineLayout, 0,
                           {*m_descriptorSets[currentImage]}, {});
  }

  void draw(vk::raii::CommandBuffer &cmd) const {
    if (indexCount > 0) {
      cmd.drawIndexed(indexCount, 1, 0, 0, 0);
    } else if (!m_vertices_data.empty()) {
    } else {
    }
  }

  [[nodiscard]] std::expected<void, std::string>
  setImageCount(u32 newCount, const vk::raii::DescriptorPool &pool,
                const vk::raii::DescriptorSetLayout &layout) NOEXCEPT {
    if (newCount == m_imageCount_member) {
      return {};
    }
    m_imageCount_member = newCount;
    EXPECTED_VOID(createMvpUniformBuffers());
    EXPECTED_VOID(createSingleMaterialUniformBuffer());
    EXPECTED_VOID(allocateDescriptorSets(pool, layout));
    return {};
  }
  Material &getMaterial() { return material; }
  [[nodiscard]] const std::string &getName() const { return name; }
};
