module;

#include "macros.hpp"
#include "primitive_types.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

export module vulkan_app:TextSystem;

import vulkan_hpp;
import std;

import :VulkanDevice;
import :VulkanPipeline;
import :VMA;
import :texture;
import :text;
import :ui;

export struct TextToggles {
  float sdfWeight{0.0};
  float pxRangeDirectAdditive{0.78};
  i32 antiAliasingMode{2}; // Corresponds to AA_LINEAR by default
  float startFadePx{0.5};
  float endFadePx{1.0};
  i32 numStages{1};
  i32 roundingDirection{2}; // 0=down, 1=up, 2=nearest
};

struct TextPushConstants2D {
  glm::mat4 projection{};
  TextToggles textToggles;
};

// This class is responsible for laying out text and preparing it for rendering.
export class TextSystem {
private:
  VulkanDevice *m_device{};
  u32 m_frameCount{};
  u32 m_maxQuadsPerFrame{2048};
  size_t m_minStorageBufferOffsetAlignment{0};
  std::vector<std::unique_ptr<Font>> m_registeredFonts;
  std::vector<VmaBuffer> m_instanceBuffers;
  using InstanceVector = std::vector<TextInstanceData>;
  std::flat_map<Font *, InstanceVector> m_frameBatch;
  VmaBuffer m_staticVertexBuffer;
  VmaBuffer m_staticIndexBuffer;
  std::vector<vk::raii::DescriptorSet> m_instanceDataDescriptorSets;
  vk::raii::DescriptorSetLayout m_instanceDataLayout{nullptr};

public:
  // Constructor now takes only essential parameters, no heavy initialization.
  TextSystem(VulkanDevice &dev, u32 inFlightFrameCount)
      : m_device(&dev), m_frameCount(inFlightFrameCount),
        m_minStorageBufferOffsetAlignment(dev.limits().minStorageBufferOffsetAlignment) {}
  TextSystem() = default;

  // Private initialization function to handle fallible setup steps.
  [[nodiscard]] std::expected<void, std::string> initialize(const vk::raii::DescriptorPool &pool) {
    if (auto res = createInstanceBuffers(*m_device, m_frameCount, m_maxQuadsPerFrame,
                                         m_instanceBuffers, sizeof(TextInstanceData));
        !res) {
      return std::unexpected("Failed to create text instance buffers: " + res.error());
    }
    if (auto res = createInstanceDataDescriptorSetLayout(); !res) {
      return std::unexpected(res.error());
    }
    if (auto res = allocateDescriptorSets(pool); !res) {
      return std::unexpected(res.error());
    }
    if (auto res = createStaticQuadBuffers(*m_device, m_staticVertexBuffer, m_staticIndexBuffer);
        !res) {
      return std::unexpected("Failed to create static quad buffers: " + res.error());
    }
    return {};
  }

  // Factory function for safe, two-phase initialization.
  // Returns a fully initialized object by value or an error.
  [[nodiscard]] static std::expected<TextSystem, std::string>
  create(VulkanDevice &dev, u32 inFlightFrameCount, const vk::raii::DescriptorPool &pool) {
    TextSystem system(dev, inFlightFrameCount);
    if (auto res = system.initialize(pool); !res) {
      return std::unexpected(res.error());
    }
    return std::move(system);
  }

  // Ensure the class is non-copyable but movable, as it manages GPU resources.
  TextSystem(const TextSystem &) = delete;
  TextSystem &operator=(const TextSystem &) = delete;
  TextSystem(TextSystem &&) = default;
  // TextSystem &operator=(TextSystem &&) = default;
  TextSystem &operator=(TextSystem &&other) noexcept {
    if (this != &other) {
      // Release current resources
      // For vk::raii objects, their destructors handle resource release
      // For VmaBuffer, ensure proper destruction or move semantics if not handled by RAII
      // In this case, VmaBuffer is likely RAII, so simply move assigning will call its operator=
      // which should handle the underlying VMA allocation correctly.

      m_device = other.m_device;
      m_frameCount = other.m_frameCount;
      m_maxQuadsPerFrame = other.m_maxQuadsPerFrame;
      m_minStorageBufferOffsetAlignment = other.m_minStorageBufferOffsetAlignment;

      // Move vectors and flat_map
      m_registeredFonts = std::move(other.m_registeredFonts);
      m_instanceBuffers = std::move(other.m_instanceBuffers);
      m_frameBatch = std::move(other.m_frameBatch);

      // Move VmaBuffers (assuming they have proper move assignment)
      m_staticVertexBuffer = std::move(other.m_staticVertexBuffer);
      m_staticIndexBuffer = std::move(other.m_staticIndexBuffer);

      // Move vk::raii::DescriptorSet and vk::raii::DescriptorSetLayout
      m_instanceDataDescriptorSets = std::move(other.m_instanceDataDescriptorSets);
      m_instanceDataLayout = std::move(other.m_instanceDataLayout);

      // Clear the moved-from object's pointers/resources to prevent double-free
      other.m_device = nullptr;
      other.m_frameCount = 0;
      other.m_maxQuadsPerFrame = 0;
      other.m_minStorageBufferOffsetAlignment = 0;
    }
    return *this;
  }
  ~TextSystem() = default;

  void beginFrame() { m_frameBatch.clear(); }

  const vk::SamplerCreateInfo MSDF_FONT_SAMPLER_CREATE_INFO{
      .magFilter = vk::Filter::eLinear,
      .minFilter = vk::Filter::eLinear,
      .mipmapMode = vk::SamplerMipmapMode::eLinear, // Use Linear for smoother scaling
      .addressModeU = vk::SamplerAddressMode::eClampToEdge,
      .addressModeV = vk::SamplerAddressMode::eClampToEdge,
      .addressModeW = vk::SamplerAddressMode::eClampToEdge,
      .mipLodBias = 0.0,
      .anisotropyEnable = vk::True, // Anisotropy can help with slanted views of text
      .maxAnisotropy = 4.0,         // A modest value
      .compareEnable = vk::False,
      .compareOp = vk::CompareOp::eAlways,
      .minLod = 0.0,
      .maxLod = vk::LodClampNone, // Allow sampler to use all mip levels if they were generated
      .borderColor = vk::BorderColor::eFloatTransparentBlack,
      .unnormalizedCoordinates = vk::False,
  };

  [[nodiscard]] std::expected<Font *, std::string>
  registerFont(std::string fontPath, const vk::raii::DescriptorSetLayout &textureLayout) {
    auto font = std::make_unique<Font>();
    auto atlasResult = createFontAtlasMSDF(fontPath);
    if (!atlasResult) {
      return std::unexpected("Failed to create font atlas: " + atlasResult.error());
    }
    font->atlasData = std::move(*atlasResult);

    auto texResult = createTexture(
        *m_device, font->atlasData.atlasBitmap.data(), font->atlasData.atlasBitmap.size(),
        vk::Extent3D{.width = (u32)font->atlasData.atlasWidth,
                     .height = (u32)font->atlasData.atlasHeight,
                     .depth = 1},
        vk::Format::eR8G8B8A8Unorm, m_device->graphicsQueue(),
        false, // generateMipmaps = false for MSDF
        {}, {}, 1, vk::ImageViewType::e2D, &MSDF_FONT_SAMPLER_CREATE_INFO);

    if (!texResult) {
      return std::unexpected("Failed to create font texture: " + texResult.error());
    }
    font->texture = std::make_shared<Texture>(std::move(*texResult));

    vk::DescriptorSetAllocateInfo allocInfo{.descriptorPool = m_device->descriptorPool(),
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &*textureLayout};
    auto setResult = m_device->logical().allocateDescriptorSets(allocInfo);
    if (!setResult) {
      return std::unexpected("Failed to allocate font descriptor set.");
    }
    font->textureDescriptorSet = std::move(setResult.value().front());
    vk::DescriptorImageInfo imageInfo{.sampler = *font->texture->sampler,
                                      .imageView = *font->texture->view,
                                      .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    vk::WriteDescriptorSet write{.dstSet = *font->textureDescriptorSet,
                                 .dstBinding = 0,
                                 .descriptorCount = 1,
                                 .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                 .pImageInfo = &imageInfo};
    m_device->logical().updateDescriptorSets({write}, nullptr);
    m_registeredFonts.push_back(std::move(font));
    return m_registeredFonts.back().get();
  }

  constexpr const static f64 SYSTEM_DPI = 96.0;
  constexpr static f64 INCH = 72.0;
  constexpr static f64 MULT = SYSTEM_DPI / INCH * 4;

  void queueText(Font *font, const std::string &text, u32 pointSize, f64 x, f64 y,
                 const glm::vec4 &color) {
    if (font == nullptr || text.empty()) {
      return;
    }

    const auto &metrics = font->atlasData;
    const f64 BASELINE_Y = y;

    auto &instanceVec = m_frameBatch[font];
    f64 cursorX = x;

    const f64 DESIRED_PIXEL_HEIGHT_FOR_EM = static_cast<f64>(pointSize) * MULT;
    const f64 FONT_UNIT_TO_PIXEL_SCALE =
        DESIRED_PIXEL_HEIGHT_FOR_EM / static_cast<f64>(metrics.unitsPerEm);

    for (char c : text) {
      auto it = metrics.glyphs.find(static_cast<u32>(c));
      if (it == metrics.glyphs.end()) {
        it = metrics.glyphs.find(static_cast<u32>('?')); // Fallback
        if (it == metrics.glyphs.end()) {
          continue;
        }
      }

      const auto &gi = it->second;

      f64 scaledQuadWidthD = static_cast<f64>(gi.width) * FONT_UNIT_TO_PIXEL_SCALE;
      f64 scaledQuadHeightD = static_cast<f64>(gi.height) * FONT_UNIT_TO_PIXEL_SCALE;

      f64 xposD = cursorX + (static_cast<f64>(gi.bearingX) * FONT_UNIT_TO_PIXEL_SCALE);
      f64 yposD = BASELINE_Y - (static_cast<f64>(gi.bearingY) * FONT_UNIT_TO_PIXEL_SCALE);

      f64 shaderPxRange = static_cast<f64>(metrics.pxRange * FONT_UNIT_TO_PIXEL_SCALE);

      instanceVec.emplace_back(TextInstanceData{
          .screenPos = {static_cast<float>(xposD), static_cast<float>(yposD)},
          .size = {static_cast<float>(scaledQuadWidthD), static_cast<float>(scaledQuadHeightD)},
          .uvTopLeft = {gi.uvX0, gi.uvY0},
          .uvBottomRight = {gi.uvX1, gi.uvY1},
          .color = color,
          .pxRange = static_cast<float>(shaderPxRange),
      });

      cursorX += static_cast<f64>(gi.advance) * FONT_UNIT_TO_PIXEL_SCALE;
    }
  }

  void prepareBatches(RenderQueue &queue, const VulkanPipeline &textPipeline, u32 frameIndex,
                      vk::Extent2D windowSize, TextToggles textToggles) {
    if (m_frameBatch.empty()) {
      return;
    }

    glm::mat4 ortho = glm::ortho(0.0F, (float)windowSize.width, 0.0F, (float)windowSize.height);

    u32 currentFirstInstance = 0;
    uint32_t currentByteOffset = 0;
    VmaBuffer &currentInstanceBuffer = m_instanceBuffers[frameIndex];

    for (auto const &[font, instances] : m_frameBatch) {
      if (instances.empty()) {
        continue;
      }

      size_t dataSize = instances.size() * sizeof(TextInstanceData);
      if (currentByteOffset + dataSize > currentInstanceBuffer.getAllocationInfo().size) {
        // Log an error or handle buffer full gracefully
        std::println("Warning: Running out of buffer space for text instances. Some text might not "
                     "be rendered.");
        break;
      }
      std::memcpy(static_cast<char *>(currentInstanceBuffer.getMappedData()) + currentByteOffset,
                  instances.data(), dataSize);

      RenderBatch batch;
      batch.sortKey = 200;
      batch.pipeline = &textPipeline.pipeline;
      batch.pipelineLayout = &textPipeline.pipelineLayout;
      batch.instanceDataSet = &m_instanceDataDescriptorSets[frameIndex];
      batch.textureSet = &font->textureDescriptorSet;
      batch.vertexBuffer = &m_staticVertexBuffer;
      batch.indexBuffer = &m_staticIndexBuffer;
      batch.indexCount = 6;
      batch.instanceCount = static_cast<u32>(instances.size());
      batch.firstInstance = 0;
      batch.dynamicOffset = currentByteOffset;

      TextPushConstants2D pc;
      pc.projection = ortho;
      pc.textToggles = textToggles;

      batch.pushConstantSize = sizeof(TextPushConstants2D);
      batch.pushConstantStages =
          vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
      std::memcpy(batch.pushConstantData.data(), &pc, sizeof(TextPushConstants2D));

      queue.push_back(batch);

      currentFirstInstance += static_cast<u32>(instances.size());
      // Ensure pad_uniform_buffer_size is defined and accessible
      size_t alignedSize = padUniformBufferSize(dataSize, this->m_minStorageBufferOffsetAlignment);
      currentByteOffset += static_cast<u32>(alignedSize);

      if (currentByteOffset > currentInstanceBuffer.getAllocationInfo().size) {
        std::println("run out of buffer space after alignment"); // This condition should ideally be
                                                                 // caught by the earlier check
        break;
      }
    }
    // Only update descriptor sets if there were instances processed
    if (currentFirstInstance > 0) {
      updateDescriptorSets(currentFirstInstance, frameIndex);
    }
  }

private:
  [[nodiscard]] std::expected<void, std::string> createInstanceDataDescriptorSetLayout() {
    vk::DescriptorSetLayoutBinding instanceBinding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eStorageBufferDynamic,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eVertex}; // Also needed in fragment
    vk::DescriptorSetLayoutCreateInfo layoutInfo{.bindingCount = 1, .pBindings = &instanceBinding};
    auto layoutResult = m_device->logical().createDescriptorSetLayout(layoutInfo);
    if (!layoutResult) {
      return std::unexpected("Failed to create text instance data descriptor set layout.");
    }
    m_instanceDataLayout = std::move(layoutResult.value());
    return {};
  }

  [[nodiscard]] std::expected<void, std::string>
  allocateDescriptorSets(const vk::raii::DescriptorPool &pool) {
    std::vector<vk::DescriptorSetLayout> layouts(m_frameCount, *m_instanceDataLayout);
    vk::DescriptorSetAllocateInfo instanceAllocInfo{
        .descriptorPool = pool, .descriptorSetCount = m_frameCount, .pSetLayouts = layouts.data()};
    auto instanceSetResult = m_device->logical().allocateDescriptorSets(instanceAllocInfo);
    if (!instanceSetResult) {
      return std::unexpected("Failed to allocate text instance descriptor sets.");
    }
    m_instanceDataDescriptorSets = std::move(instanceSetResult.value());
    return {};
  }

  void updateDescriptorSets(u32 instanceNumber, u32 frameIndex) {
    vk::DescriptorBufferInfo bufferInfo{
        .buffer = m_instanceBuffers[frameIndex].get(),
        .offset = 0,
        .range = instanceNumber * sizeof(TextInstanceData),
    };
    vk::WriteDescriptorSet instanceWrite{.dstSet = m_instanceDataDescriptorSets[frameIndex],
                                         .dstBinding = 0,
                                         .dstArrayElement = 0,
                                         .descriptorCount = 1,
                                         .descriptorType =
                                             vk::DescriptorType::eStorageBufferDynamic,
                                         .pBufferInfo = &bufferInfo};
    m_device->logical().updateDescriptorSets({instanceWrite}, nullptr);
  }
};
