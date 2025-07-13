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
  float sdf_weight{0.0};
  float pxRangeDirectAdditive{0.78};
  i32 antiAliasingMode{2}; // Corresponds to AA_LINEAR by default
  float start_fade_px{0.5};
  float end_fade_px{1.0};
  i32 num_stages{1};
  i32 rounding_direction{2}; // 0=down, 1=up, 2=nearest
};

struct TextPushConstants2D {
  glm::mat4 projection;
  TextToggles textToggles;
};

// This class is responsible for laying out text and preparing it for rendering.
export class TextSystem {
private:
  VulkanDevice *device{};
  u32 frameCount{};
  u32 maxQuadsPerFrame{2048};
  size_t minStorageBufferOffsetAlignment{0};
  std::vector<std::unique_ptr<Font>> registeredFonts;
  std::vector<VmaBuffer> instanceBuffers;
  using InstanceVector = std::vector<TextInstanceData>;
  std::flat_map<Font *, InstanceVector> frameBatch;
  VmaBuffer staticVertexBuffer;
  VmaBuffer staticIndexBuffer;
  std::vector<vk::raii::DescriptorSet> instanceDataDescriptorSets;
  vk::raii::DescriptorSetLayout instanceDataLayout{nullptr};

public:
  // Constructor now takes only essential parameters, no heavy initialization.
  TextSystem(VulkanDevice &dev, u32 inFlightFrameCount)
      : device(&dev), frameCount(inFlightFrameCount),
        minStorageBufferOffsetAlignment(dev.limits.minStorageBufferOffsetAlignment) {}
  TextSystem() = default;

  // Private initialization function to handle fallible setup steps.
  [[nodiscard]] std::expected<void, std::string> initialize(const vk::raii::DescriptorPool &pool) {
    if (auto res = createInstanceBuffers(*device, frameCount, maxQuadsPerFrame, instanceBuffers,
                                         sizeof(TextInstanceData));
        !res) {
      return std::unexpected("Failed to create text instance buffers: " + res.error());
    }
    if (auto res = createInstanceDataDescriptorSetLayout(); !res) {
      return std::unexpected(res.error());
    }
    if (auto res = allocateDescriptorSets(pool); !res) {
      return std::unexpected(res.error());
    }
    if (auto res = createStaticQuadBuffers(*device, staticVertexBuffer, staticIndexBuffer); !res) {
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

      device = other.device;
      frameCount = other.frameCount;
      maxQuadsPerFrame = other.maxQuadsPerFrame;
      minStorageBufferOffsetAlignment = other.minStorageBufferOffsetAlignment;

      // Move vectors and flat_map
      registeredFonts = std::move(other.registeredFonts);
      instanceBuffers = std::move(other.instanceBuffers);
      frameBatch = std::move(other.frameBatch);

      // Move VmaBuffers (assuming they have proper move assignment)
      staticVertexBuffer = std::move(other.staticVertexBuffer);
      staticIndexBuffer = std::move(other.staticIndexBuffer);

      // Move vk::raii::DescriptorSet and vk::raii::DescriptorSetLayout
      instanceDataDescriptorSets = std::move(other.instanceDataDescriptorSets);
      instanceDataLayout = std::move(other.instanceDataLayout);

      // Clear the moved-from object's pointers/resources to prevent double-free
      other.device = nullptr;
      other.frameCount = 0;
      other.maxQuadsPerFrame = 0;
      other.minStorageBufferOffsetAlignment = 0;
    }
    return *this;
  }
  ~TextSystem() = default;

  void beginFrame() { frameBatch.clear(); }

  const vk::SamplerCreateInfo msdfFontSamplerCreateInfo{
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
  registerFont(const std::string &fontPath, int pixelHeight,
               const vk::raii::DescriptorSetLayout &textureLayout) {
    auto font = std::make_unique<Font>();
    auto atlasResult = createFontAtlasMSDF(fontPath, pixelHeight);
    if (!atlasResult) {
      return std::unexpected("Failed to create font atlas: " + atlasResult.error());
    }
    font->atlasData = std::move(*atlasResult);

    auto texResult = createTexture(*device, font->atlasData.atlasBitmap.data(),
                                   font->atlasData.atlasBitmap.size(),
                                   vk::Extent3D{.width = (u32)font->atlasData.atlasWidth,
                                                .height = (u32)font->atlasData.atlasHeight,
                                                .depth = 1},
                                   vk::Format::eR8G8B8A8Unorm, device->queue_,
                                   false, // generateMipmaps = false for MSDF
                                   {}, {}, 1, vk::ImageViewType::e2D, &msdfFontSamplerCreateInfo);

    if (!texResult) {
      return std::unexpected("Failed to create font texture: " + texResult.error());
    }
    font->texture = std::make_shared<Texture>(std::move(*texResult));

    vk::DescriptorSetAllocateInfo allocInfo{.descriptorPool = device->descriptorPool_,
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &*textureLayout};
    auto setResult = device->logical().allocateDescriptorSets(allocInfo);
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
    device->logical().updateDescriptorSets({write}, nullptr);
    registeredFonts.push_back(std::move(font));
    return registeredFonts.back().get();
  }

  constexpr const static f64 SYSTEM_DPI = 96.0;
  constexpr static f64 inch = 72.0;
  constexpr static f64 mult = SYSTEM_DPI / inch * 4;

  void queueText(Font *font, const std::string &text, u32 pointSize, f64 x, f64 y,
                 const glm::vec4 &color) {
    if (font == nullptr || text.empty()) {
      return;
    }

    const auto &metrics = font->atlasData;
    const f64 baselineY = y;

    auto &instanceVec = frameBatch[font];
    f64 cursorX = x;

    const f64 desiredPixelHeightForEm = static_cast<f64>(pointSize) * mult;
    const f64 fontUnitToPixelScale = desiredPixelHeightForEm / static_cast<f64>(metrics.unitsPerEm);

    for (char c : text) {
      auto it = metrics.glyphs.find(static_cast<u32>(c));
      if (it == metrics.glyphs.end()) {
        it = metrics.glyphs.find(static_cast<u32>('?')); // Fallback
        if (it == metrics.glyphs.end()) {
          continue;
        }
      }

      const auto &gi = it->second;

      f64 scaledQuadWidth_d = static_cast<f64>(gi.width) * fontUnitToPixelScale;
      f64 scaledQuadHeight_d = static_cast<f64>(gi.height) * fontUnitToPixelScale;

      f64 xpos_d = cursorX + (static_cast<f64>(gi.bearing_x) * fontUnitToPixelScale);
      f64 ypos_d = baselineY - (static_cast<f64>(gi.bearing_y) * fontUnitToPixelScale);

      f64 shaderPxRange = static_cast<f64>(metrics.pxRange * fontUnitToPixelScale);

      instanceVec.emplace_back(TextInstanceData{
          .screenPos = {static_cast<float>(xpos_d), static_cast<float>(ypos_d)},
          .size = {static_cast<float>(scaledQuadWidth_d), static_cast<float>(scaledQuadHeight_d)},
          .uvTopLeft = {gi.uv_x0, gi.uv_y0},
          .uvBottomRight = {gi.uv_x1, gi.uv_y1},
          .color = color,
          .pxRange = static_cast<float>(shaderPxRange),
      });

      cursorX += static_cast<f64>(gi.advance) * fontUnitToPixelScale;
    }
  }

  void prepareBatches(RenderQueue &queue, const VulkanPipeline &textPipeline, u32 frameIndex,
                      vk::Extent2D windowSize, TextToggles textToggles) {
    if (frameBatch.empty()) {
      return;
    }

    glm::mat4 ortho = glm::ortho(0.0F, (float)windowSize.width, 0.0F, (float)windowSize.height);

    u32 currentFirstInstance = 0;
    uint32_t currentByteOffset = 0;
    VmaBuffer &currentInstanceBuffer = instanceBuffers[frameIndex];

    for (auto const &[font, instances] : frameBatch) {
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
      batch.instanceDataSet = &instanceDataDescriptorSets[frameIndex];
      batch.textureSet = &font->textureDescriptorSet;
      batch.vertexBuffer = &staticVertexBuffer;
      batch.indexBuffer = &staticIndexBuffer;
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
      size_t alignedSize = pad_uniform_buffer_size(dataSize, this->minStorageBufferOffsetAlignment);
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
    auto layoutResult = device->logical().createDescriptorSetLayout(layoutInfo);
    if (!layoutResult) {
      return std::unexpected("Failed to create text instance data descriptor set layout.");
    }
    instanceDataLayout = std::move(layoutResult.value());
    return {};
  }

  [[nodiscard]] std::expected<void, std::string>
  allocateDescriptorSets(const vk::raii::DescriptorPool &pool) {
    std::vector<vk::DescriptorSetLayout> layouts(frameCount, *instanceDataLayout);
    vk::DescriptorSetAllocateInfo instanceAllocInfo{
        .descriptorPool = pool, .descriptorSetCount = frameCount, .pSetLayouts = layouts.data()};
    auto instanceSetResult = device->logical().allocateDescriptorSets(instanceAllocInfo);
    if (!instanceSetResult) {
      return std::unexpected("Failed to allocate text instance descriptor sets.");
    }
    instanceDataDescriptorSets = std::move(instanceSetResult.value());
    return {};
  }

  void updateDescriptorSets(u32 instanceNumber, u32 frameIndex) {
    vk::DescriptorBufferInfo bufferInfo{
        .buffer = instanceBuffers[frameIndex].get(),
        .offset = 0,
        .range = instanceNumber * sizeof(TextInstanceData),
    };
    vk::WriteDescriptorSet instanceWrite{.dstSet = instanceDataDescriptorSets[frameIndex],
                                         .dstBinding = 0,
                                         .dstArrayElement = 0,
                                         .descriptorCount = 1,
                                         .descriptorType =
                                             vk::DescriptorType::eStorageBufferDynamic,
                                         .pBufferInfo = &bufferInfo};
    device->logical().updateDescriptorSets({instanceWrite}, nullptr);
  }
};
