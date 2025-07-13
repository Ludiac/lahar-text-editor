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
  VulkanDevice &device_;
  u32 imageCount_member;

  std::vector<Vertex> vertices_data;
  std::vector<u32> indices_data;

  VmaBuffer vertexBuffer;
  VmaBuffer indexBuffer;
  VmaBuffer mvpUniformBuffers;
  VmaBuffer materialUniformBuffer;

  std::vector<vk::raii::DescriptorSet> descriptorSets;

  [[nodiscard]] std::expected<void, std::string> createVertexBuffer() NOEXCEPT {
    if (vertices_data.empty())
      return {};
    const auto bufferSize = sizeof(Vertex) * vertices_data.size();

    vk::BufferCreateInfo bufferInfo{.size = bufferSize,
                                    .usage = vk::BufferUsageFlagBits::eVertexBuffer

    };
    vma::AllocationCreateInfo allocInfo{
        .flags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite |
                 vma::AllocationCreateFlagBits::eMapped,
        .usage = vma::MemoryUsage::eAutoPreferHost

    };

    auto bufferResult = device_.createBufferVMA(bufferInfo, allocInfo);
    if (!bufferResult)
      return std::unexpected("Mesh '" + name +
                             "': Vertex VmaBuffer creation: " + bufferResult.error());
    vertexBuffer = std::move(bufferResult.value());

    if (!vertexBuffer.getMappedData())
      return std::unexpected("Mesh '" + name + "': Vertex VmaBuffer not mapped after creation.");

    std::memcpy(vertexBuffer.getMappedData(), vertices_data.data(), bufferSize);

    return {};
  }

  [[nodiscard]] std::expected<void, std::string> createIndexBuffer() NOEXCEPT {
    if (indices_data.empty()) {
      this->indexCount = 0;
      return {};
    }
    this->indexCount = static_cast<u32>(indices_data.size());
    const auto bufferSize = sizeof(u32) * indices_data.size();

    vk::BufferCreateInfo bufferInfo{.size = bufferSize,
                                    .usage = vk::BufferUsageFlagBits::eIndexBuffer};
    vma::AllocationCreateInfo allocInfo{
        .flags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite |
                 vma::AllocationCreateFlagBits::eMapped,
        .usage = vma::MemoryUsage::eAutoPreferHost};
    auto bufferResult = device_.createBufferVMA(bufferInfo, allocInfo);
    if (!bufferResult)
      return std::unexpected("Mesh '" + name +
                             "': Index VmaBuffer creation: " + bufferResult.error());
    indexBuffer = std::move(bufferResult.value());

    if (!indexBuffer.getMappedData())
      return std::unexpected("Mesh '" + name + "': Index VmaBuffer not mapped.");
    std::memcpy(indexBuffer.getMappedData(), indices_data.data(), bufferSize);
    return {};
  }

  [[nodiscard]] std::expected<void, std::string> createMvpUniformBuffers() NOEXCEPT {
    if (imageCount_member == 0)
      return {};
    auto bufferSize = sizeof(UniformBufferObject) * imageCount_member;
    if (bufferSize == 0)
      return {};

    vk::BufferCreateInfo bufferInfo{.size = bufferSize,
                                    .usage = vk::BufferUsageFlagBits::eUniformBuffer};
    vma::AllocationCreateInfo allocInfo{
        .flags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite |
                 vma::AllocationCreateFlagBits::eMapped,
        .usage = vma::MemoryUsage::eAutoPreferHost};
    auto bufferResult = device_.createBufferVMA(bufferInfo, allocInfo);
    if (!bufferResult)
      return std::unexpected("Mesh '" + name +
                             "': MVP UBO VmaBuffer creation: " + bufferResult.error());
    mvpUniformBuffers = std::move(bufferResult.value());
    return {};
  }

  [[nodiscard]] std::expected<void, std::string> createSingleMaterialUniformBuffer() NOEXCEPT {
    if (imageCount_member == 0)
      return {};
    auto alignment = device_.physical().getProperties().limits.minUniformBufferOffsetAlignment;
    auto alignedMaterialSize = (sizeof(Material) + alignment - 1) & ~(alignment - 1);
    auto bufferSize = alignedMaterialSize * imageCount_member;
    if (bufferSize == 0)
      return {};

    vk::BufferCreateInfo bufferInfo{.size = bufferSize,
                                    .usage = vk::BufferUsageFlagBits::eUniformBuffer};
    vma::AllocationCreateInfo allocInfo{
        .flags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite |
                 vma::AllocationCreateFlagBits::eMapped,
        .usage = vma::MemoryUsage::eAutoPreferHost};
    auto bufferResult = device_.createBufferVMA(bufferInfo, allocInfo);
    if (!bufferResult)
      return std::unexpected("Mesh '" + name +
                             "': Material UBO VmaBuffer creation: " + bufferResult.error());
    materialUniformBuffer = std::move(bufferResult.value());

    for (u32 i = 0; i < imageCount_member; ++i) {
      updateMaterialUniformBufferData(i);
    }
    return {};
  }

public:
  Mesh(VulkanDevice &dev, std::string meshName, std::vector<Vertex> &&verts,
       std::vector<u32> &&meshIndices, Material initMaterial, PBRTextures initPbrTextures,
       u32 numImages)
      : device_(dev), name(std::move(meshName)), material(std::move(initMaterial)),
        textures(std::move(initPbrTextures)), imageCount_member(numImages),
        vertices_data(std::move(verts)), indices_data(std::move(meshIndices)) {
    EXPECTED_VOID(createVertexBuffer());
    EXPECTED_VOID(createIndexBuffer());
    EXPECTED_VOID(createMvpUniformBuffers());
    EXPECTED_VOID(createSingleMaterialUniformBuffer());
  }

  void updateMaterialUniformBufferData(u32 currentImage) {
    if (!materialUniformBuffer || currentImage >= imageCount_member)
      return;
    if (!materialUniformBuffer.getMappedData()) {
      std::println("Error: Mesh '{}': Material UBO not mapped for image index {}.", name,
                   currentImage);
      return;
    }
    auto alignment = device_.physical().getProperties().limits.minUniformBufferOffsetAlignment;
    auto alignedMaterialSize = (sizeof(Material) + alignment - 1) & ~(alignment - 1);
    vk::DeviceSize offset = currentImage * alignedMaterialSize;
    char *baseMapped = static_cast<char *>(materialUniformBuffer.getMappedData());
    std::memcpy(baseMapped + offset, &this->material, sizeof(Material));
  }

  void updateMvpUniformBuffer(u32 currentImage, const glm::mat4 &model, const glm::mat4 &view,
                              const glm::mat4 &projection) {
    if (!mvpUniformBuffers || currentImage >= imageCount_member)
      return;
    if (!mvpUniformBuffers.getMappedData()) {
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
    char *baseMapped = static_cast<char *>(mvpUniformBuffers.getMappedData());
    std::memcpy(baseMapped + offset, &ubo, sizeof(ubo));
  }

  [[nodiscard]] std::expected<void, std::string>
  allocateDescriptorSets(const vk::raii::DescriptorPool &pool,
                         const vk::raii::DescriptorSetLayout &combinedLayout) {
    if (imageCount_member == 0) {
      std::string errorMsg =
          "Mesh " + name + ": Image count is zero for descriptor set allocation.";
      return std::unexpected(errorMsg);
    }
    if (!*combinedLayout) {
      std::string errorMsg =
          "Mesh " + name + ": Combined layout is null for descriptor set allocation.";
      return std::unexpected(errorMsg);
    }
    std::vector<vk::DescriptorSetLayout> layouts(imageCount_member, *combinedLayout);
    vk::DescriptorSetAllocateInfo allocInfo{
        .descriptorPool = *pool,
        .descriptorSetCount = imageCount_member,
        .pSetLayouts = layouts.data(),
    };
    auto setsResult = device_.logical().allocateDescriptorSets(allocInfo);
    if (!setsResult) {
      std::string errorMsg = "Mesh " + name + ": Failed to allocate combined descriptor sets: " +
                             vk::to_string(setsResult.error());
      return std::unexpected(errorMsg);
    }
    descriptorSets = std::move(setsResult.value());
    return {};
  }

  void updateDescriptorSetContents(u32 currentImage) {
    if (currentImage >= imageCount_member || descriptorSets.empty() ||
        currentImage >= descriptorSets.size() || !*descriptorSets[currentImage]) {
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
    if (mvpUniformBuffers) {
      mvpUboInfoStorage =
          vk::DescriptorBufferInfo{.buffer = mvpUniformBuffers.buffer_,
                                   .offset = currentImage * sizeof(UniformBufferObject),
                                   .range = sizeof(UniformBufferObject)};
      writes.emplace_back(
          vk::WriteDescriptorSet{.dstSet = *descriptorSets[currentImage],
                                 .dstBinding = 0,
                                 .dstArrayElement = 0,
                                 .descriptorCount = 1,
                                 .descriptorType = vk::DescriptorType::eUniformBuffer,
                                 .pBufferInfo = &mvpUboInfoStorage});
    } else {
    }

    if (materialUniformBuffer) {
      auto alignment = device_.physical().getProperties().limits.minUniformBufferOffsetAlignment;
      auto alignedMaterialSize = (sizeof(Material) + alignment - 1) & ~(alignment - 1);
      materialUboInfoStorage =
          vk::DescriptorBufferInfo{.buffer = materialUniformBuffer.buffer_,
                                   .offset = currentImage * alignedMaterialSize,
                                   .range = sizeof(Material)};
      writes.emplace_back(
          vk::WriteDescriptorSet{.dstSet = *descriptorSets[currentImage],
                                 .dstBinding = 1,
                                 .dstArrayElement = 0,
                                 .descriptorCount = 1,
                                 .descriptorType = vk::DescriptorType::eUniformBuffer,
                                 .pBufferInfo = &materialUboInfoStorage});
    } else {
    }

    auto create_texture_write = [&](uint32_t binding, vk::DescriptorImageInfo &imageInfo,
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
            vk::WriteDescriptorSet{.dstSet = *descriptorSets[currentImage],
                                   .dstBinding = binding,
                                   .dstArrayElement = 0,
                                   .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                   .pImageInfo = &imageInfo});
      }
    };

    create_texture_write(2, baseColorTextureInfoStorage, textures.baseColor);
    create_texture_write(3, normalTextureInfoStorage, textures.normal);
    create_texture_write(4, metallicRoughnessTextureInfoStorage, textures.metallicRoughness);
    create_texture_write(5, occlusionTextureInfoStorage, textures.occlusion);
    create_texture_write(6, emissiveTextureInfoStorage, textures.emissive);

    if (!writes.empty()) {
      device_.logical().updateDescriptorSets(writes, nullptr);
    } else {
    }
  }

  void bind(vk::raii::CommandBuffer &cmd, VulkanPipeline *pipeline, u32 currentImage) const {
    if (!vertexBuffer) {
      return;
    }
    if (!indexBuffer && indexCount > 0) {
      return;
    }
    if (descriptorSets.empty() || currentImage >= descriptorSets.size() ||
        !*descriptorSets[currentImage]) {
      return;
    }
    if (!pipeline || !*pipeline->pipelineLayout) {
      return;
    }
    cmd.bindVertexBuffers(0, {vertexBuffer.buffer_}, {0});
    if (indexCount > 0 && indexBuffer) {
      cmd.bindIndexBuffer(indexBuffer.buffer_, 0, vk::IndexType::eUint32);
    }
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline->pipelineLayout, 0,
                           {*descriptorSets[currentImage]}, {});
  }

  void draw(vk::raii::CommandBuffer &cmd) const {
    if (indexCount > 0) {
      cmd.drawIndexed(indexCount, 1, 0, 0, 0);
    } else if (!vertices_data.empty()) {
    } else {
    }
  }

  [[nodiscard]] std::expected<void, std::string>
  setImageCount(u32 newCount, const vk::raii::DescriptorPool &pool,
                const vk::raii::DescriptorSetLayout &layout) NOEXCEPT {
    if (newCount == imageCount_member) {
      return {};
    }
    imageCount_member = newCount;
    EXPECTED_VOID(createMvpUniformBuffers());
    EXPECTED_VOID(createSingleMaterialUniformBuffer());
    EXPECTED_VOID(allocateDescriptorSets(pool, layout));
    return {};
  }
  Material &getMaterial() { return material; }
  const std::string &getName() const { return name; }
};
