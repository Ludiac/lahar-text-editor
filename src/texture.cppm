module;

#include "macros.hpp"
#include "primitive_types.hpp"

export module vulkan_app:texture;

import vulkan_hpp;
import std;
import :VulkanDevice;
import :VMA;
import :utils;

export struct Texture {
  VmaImage image;
  vk::raii::ImageView view{nullptr};
  vk::raii::Sampler sampler{nullptr};

  vk::Format format{vk::Format::eUndefined};
  vk::Extent3D extent{.width = 0, .height = 0, .depth = 0};
  u32 mipLevels{0};
  u32 arrayLayers{0};
};

export struct PBRTextures {
  std::shared_ptr<Texture> baseColor;
  std::shared_ptr<Texture> metallicRoughness;
  std::shared_ptr<Texture> normal;
  std::shared_ptr<Texture> occlusion;
  std::shared_ptr<Texture> emissive;
};

namespace TextureHelpers {

void copyBufferToImage(const vk::raii::CommandBuffer &commandBuffer, vk::Buffer buffer,
                       vk::Image image, vk::Extent3D extent,
                       const vk::ImageSubresourceLayers &imageSubresource) {
  vk::BufferImageCopy region{.bufferOffset = 0,
                             .bufferRowLength = 0,
                             .bufferImageHeight = 0,
                             .imageSubresource = imageSubresource,
                             .imageOffset = {.x = 0, .y = 0, .z = 0},
                             .imageExtent = extent};
  commandBuffer.copyBufferToImage(buffer, image, vk::ImageLayout::eTransferDstOptimal, region);
}

// Generates mipmaps for a given image using blitting.
void generateMipmaps(VulkanDevice &vulkanDevice, const vk::raii::CommandBuffer &commandBuffer,
                     vk::Image image, vk::Extent2D texExtent, u32 totalMipLevels, vk::Format format,
                     u32 arrayLayers = 1, u32 baseArrayLayer = 0) {
  if (totalMipLevels <= 1) {
    return;
  }

  vk::FormatProperties formatProperties = vulkanDevice.physical().getFormatProperties(format);
  if (!(formatProperties.optimalTilingFeatures &
        vk::FormatFeatureFlagBits::eSampledImageFilterLinear)) {
    std::println("Warning: Image format {} does not support linear blitting for mipmap generation.",
                 vk::to_string(format).c_str());
  }

  vk::ImageSubresourceRange subresourceRangeBase{.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                 .baseMipLevel = 0,
                                                 .levelCount = 1,
                                                 .baseArrayLayer = baseArrayLayer,
                                                 .layerCount = arrayLayers};

  i32 mipWidth = static_cast<i32>(texExtent.width);
  i32 mipHeight = static_cast<i32>(texExtent.height);

  for (u32 i = 1; i < totalMipLevels; ++i) {
    subresourceRangeBase.baseMipLevel = i - 1;
    transitionImageLayout(commandBuffer, image, format, vk::ImageLayout::eTransferDstOptimal,
                          vk::ImageLayout::eTransferSrcOptimal, subresourceRangeBase);

    int32_t nextMipWidth = mipWidth > 1 ? mipWidth / 2 : 1;
    int32_t nextMipHeight = mipHeight > 1 ? mipHeight / 2 : 1;

    vk::ImageBlit blit{
        .srcSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                           .mipLevel = i - 1,
                           .baseArrayLayer = baseArrayLayer,
                           .layerCount = arrayLayers},
        .srcOffsets = std::array<vk::Offset3D, 2>{{{.x = 0, .y = 0, .z = 0},
                                                   {.x = mipWidth, .y = mipHeight, .z = 1}}},
        .dstSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                           .mipLevel = i,
                           .baseArrayLayer = baseArrayLayer,
                           .layerCount = arrayLayers},
        .dstOffsets = std::array<vk::Offset3D, 2>{
            {{.x = 0, .y = 0, .z = 0}, {.x = nextMipWidth, .y = nextMipHeight, .z = 1}}}};

    commandBuffer.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, image,
                            vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);

    transitionImageLayout(commandBuffer, image, format, vk::ImageLayout::eTransferSrcOptimal,
                          vk::ImageLayout::eShaderReadOnlyOptimal, subresourceRangeBase);

    mipWidth = nextMipWidth;
    mipHeight = nextMipHeight;
  }

  subresourceRangeBase.baseMipLevel = totalMipLevels - 1;
  transitionImageLayout(commandBuffer, image, format, vk::ImageLayout::eTransferDstOptimal,
                        vk::ImageLayout::eShaderReadOnlyOptimal, subresourceRangeBase);
}
} // namespace TextureHelpers

export [[nodiscard]] std::expected<Texture, std::string>
createTexture(VulkanDevice &vulkanDevice, const void *pixels, vk::DeviceSize imageSize,
              vk::Extent3D texExtent, vk::Format texFormat, const vk::raii::Queue &transferQueue,
              bool generateMipmaps = true, vk::ImageUsageFlags additionalImageUsage = {},
              vk::ImageCreateFlags imageCreateFlags = {}, u32 arrayLayers = 1,
              vk::ImageViewType viewType = vk::ImageViewType::e2D,
              const vk::SamplerCreateInfo *pCustomSamplerInfo = nullptr) {
  if (texExtent.width == 0 || texExtent.height == 0 || texExtent.depth == 0 || arrayLayers == 0) {
    return std::unexpected(
        "createTexture: Texture dimensions and arrayLayers must be greater than zero.");
  }
  if (pixels == nullptr && imageSize > 0) {
    return std::unexpected("createTexture: Pixel data is null but imageSize is non-zero.");
  }
  if (pixels != nullptr && imageSize == 0) {
    return std::unexpected("createTexture: Pixel data is provided but imageSize is zero.");
  }

  Texture textureOut;
  textureOut.arrayLayers = arrayLayers;
  textureOut.extent = texExtent;

  if (generateMipmaps && texExtent.width > 0 && texExtent.height > 0) {
    textureOut.mipLevels =
        static_cast<u32>(std::floor(std::log2(std::max(texExtent.width, texExtent.height)))) + 1;
  } else {
    textureOut.mipLevels = 1;
  }

  VmaBuffer stagingVmaBuffer;

  if (pixels != nullptr && imageSize > 0) {
    vk::BufferCreateInfo stagingBufferInfo{.size = imageSize,
                                           .usage = vk::BufferUsageFlagBits::eTransferSrc};
    vma::AllocationCreateInfo stagingAllocInfo{
        .flags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite |
                 vma::AllocationCreateFlagBits::eMapped,
        .usage = vma::MemoryUsage::eAutoPreferHost};
    auto stagingResult = vulkanDevice.createBufferVMA(stagingBufferInfo, stagingAllocInfo);
    if (!stagingResult) {
      return std::unexpected("createTexture: Staging buffer creation failed: " +
                             stagingResult.error());
    }
    stagingVmaBuffer = std::move(stagingResult.value());

    if (stagingVmaBuffer.getMappedData() == nullptr) {
      return std::unexpected("createTexture: Staging buffer not mapped.");
    }
    std::memcpy(stagingVmaBuffer.getMappedData(), pixels, static_cast<size_t>(imageSize));
  }

  vk::ImageCreateInfo imageCi{.flags = imageCreateFlags,
                              .imageType =
                                  (texExtent.depth > 1 && viewType != vk::ImageViewType::e2DArray)
                                      ? vk::ImageType::e3D
                                      : vk::ImageType::e2D,
                              .format = texFormat,
                              .extent = texExtent,
                              .mipLevels = textureOut.mipLevels,
                              .arrayLayers = textureOut.arrayLayers,
                              .samples = vk::SampleCountFlagBits::e1,
                              .tiling = vk::ImageTiling::eOptimal,
                              .usage = vk::ImageUsageFlagBits::eSampled |
                                       vk::ImageUsageFlagBits::eTransferDst | additionalImageUsage,
                              .initialLayout = vk::ImageLayout::eUndefined};
  if (generateMipmaps && textureOut.mipLevels > 1) {
    imageCi.usage |= vk::ImageUsageFlagBits::eTransferSrc;
  }

  vma::AllocationCreateInfo imageAllocInfo{
      .usage = vma::MemoryUsage::eAutoPreferDevice // Images typically device local
      // .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT (for render targets or very large
      // textures)
  };

  auto vmaImageResult = vulkanDevice.createImageVMA(imageCi, imageAllocInfo);
  if (!vmaImageResult) {
    return std::unexpected("createTexture: VMA image creation failed: " + vmaImageResult.error());
  }
  textureOut.image = std::move(vmaImageResult.value());

  auto commandBufferExpected = vulkanDevice.beginSingleTimeCommands();
  if (!commandBufferExpected) {
    return std::unexpected("createTexture: " + commandBufferExpected.error());
  }
  vk::raii::CommandBuffer commandBuffer = std::move(*commandBufferExpected);

  vk::ImageSubresourceRange baseSubresourceRange{.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                 .baseMipLevel = 0,
                                                 .levelCount = textureOut.mipLevels,
                                                 .baseArrayLayer = 0,
                                                 .layerCount = textureOut.arrayLayers};
  if (additionalImageUsage & vk::ImageUsageFlagBits::eDepthStencilAttachment ||
      texFormat == vk::Format::eD32Sfloat || texFormat == vk::Format::eD16Unorm ||
      texFormat == vk::Format::eD32SfloatS8Uint || texFormat == vk::Format::eD24UnormS8Uint ||
      texFormat == vk::Format::eD16UnormS8Uint) {
    baseSubresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
    if (texFormat == vk::Format::eD32SfloatS8Uint || texFormat == vk::Format::eD24UnormS8Uint ||
        texFormat == vk::Format::eD16UnormS8Uint) {
      baseSubresourceRange.aspectMask |= vk::ImageAspectFlagBits::eStencil;
    }
  }

  if (pixels != nullptr && imageSize > 0) {
    transitionImageLayout(commandBuffer, textureOut.image.get(), textureOut.format,
                          vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                          baseSubresourceRange);

    vk::ImageSubresourceLayers copySubresourceLayers{.aspectMask = baseSubresourceRange.aspectMask,
                                                     .mipLevel = 0,
                                                     .baseArrayLayer = 0,
                                                     .layerCount = textureOut.arrayLayers};
    TextureHelpers::copyBufferToImage(commandBuffer, stagingVmaBuffer.get(), textureOut.image.get(),
                                      texExtent, copySubresourceLayers);

    if (generateMipmaps && textureOut.mipLevels > 1) {
      TextureHelpers::generateMipmaps(
          vulkanDevice, commandBuffer, textureOut.image.get(),
          vk::Extent2D{.width = texExtent.width, .height = texExtent.height}, textureOut.mipLevels,
          textureOut.format, textureOut.arrayLayers);
    } else {
      transitionImageLayout(commandBuffer, textureOut.image.get(), textureOut.format,
                            vk::ImageLayout::eTransferDstOptimal,
                            vk::ImageLayout::eShaderReadOnlyOptimal, baseSubresourceRange);
    }
  } else {
    vk::ImageLayout targetLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    if (additionalImageUsage & vk::ImageUsageFlagBits::eColorAttachment) {
      targetLayout = vk::ImageLayout::eColorAttachmentOptimal;
    } else if (additionalImageUsage & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
      targetLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    } else if (additionalImageUsage & vk::ImageUsageFlagBits::eStorage) {
      targetLayout = vk::ImageLayout::eGeneral;
    }
    transitionImageLayout(commandBuffer, textureOut.image.get(), textureOut.format,
                          vk::ImageLayout::eUndefined, targetLayout, baseSubresourceRange);
  }

  auto endCommandsExpected = vulkanDevice.endSingleTimeCommands(std::move(commandBuffer));
  if (!endCommandsExpected) {
    return std::unexpected("createTexture: " + endCommandsExpected.error());
  }

  vk::ImageViewCreateInfo viewInfo{
      .image = textureOut.image.get(),
      .viewType = viewType,
      .format = textureOut.image.getFormat(),

      .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                           .baseMipLevel = 0,
                           .levelCount = textureOut.mipLevels,
                           .baseArrayLayer = 0,
                           .layerCount = textureOut.arrayLayers}};
  auto viewResult = vulkanDevice.logical().createImageView(viewInfo);
  if (!viewResult) {
    return std::unexpected("createTexture: Failed to create ImageView: " +
                           vk::to_string(viewResult.error()));
  }
  textureOut.view = std::move(*viewResult);

  if (pCustomSamplerInfo != nullptr) {
    auto samplerResultValue = vulkanDevice.logical().createSampler(*pCustomSamplerInfo);
    if (!samplerResultValue) {
      return std::unexpected("createTexture: Failed to create custom sampler: " +
                             vk::to_string(samplerResultValue.error()));
    }
    textureOut.sampler = std::move(samplerResultValue.value());
  } else {
    vk::SamplerCreateInfo samplerInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eLinear,
        .addressModeU = vk::SamplerAddressMode::eRepeat,
        .addressModeV = vk::SamplerAddressMode::eRepeat,
        .addressModeW = vk::SamplerAddressMode::eRepeat,
        .mipLodBias = 0.0,
        .anisotropyEnable = vulkanDevice.physical().getFeatures().samplerAnisotropy == vk::True
                                ? vk::True
                                : vk::False,
        .maxAnisotropy = static_cast<float>(
            vulkanDevice.physical().getFeatures().samplerAnisotropy == vk::True
                ? vulkanDevice.physical().getProperties().limits.maxSamplerAnisotropy
                : 1.0),
        .compareEnable = vk::False,
        .compareOp = vk::CompareOp::eAlways,
        .minLod = 0.0,
        .maxLod = static_cast<float>(textureOut.mipLevels),
        .borderColor = vk::BorderColor::eFloatOpaqueBlack,
        .unnormalizedCoordinates = vk::False};
    auto samplerResultValue = vulkanDevice.logical().createSampler(samplerInfo);
    if (!samplerResultValue) {
      return std::unexpected("createTexture: Failed to create default sampler: " +
                             vk::to_string(samplerResultValue.error()));
    }
    textureOut.sampler = std::move(samplerResultValue.value());
  }

  return textureOut;
}

// Creates a simple, default 1x1 texture with a specified color.
export [[nodiscard]] std::expected<Texture, std::string>
createDefaultTexture(VulkanDevice &vulkanDevice, const vk::raii::Queue &transferQueue,
                     vk::Format format = vk::Format::eR8G8B8A8Unorm,
                     std::array<uint8_t, 4> color = {255, 255, 255, 255},
                     const vk::SamplerCreateInfo *pCustomSamplerInfo = nullptr) {
  vk::Extent3D extent = {.width = 1, .height = 1, .depth = 1};
  std::vector<uint8_t> pixels;
  u32 bytesPerPixel = 0;

  switch (format) {
  case vk::Format::eR8G8B8A8Unorm:
  case vk::Format::eR8G8B8A8Srgb:
    bytesPerPixel = 4;
    pixels.resize(bytesPerPixel);
    pixels[0] = color[0];
    pixels[1] = color[1];
    pixels[2] = color[2];
    pixels[3] = color[3];
    break;
  default:
    return std::unexpected(
        "createDefaultTexture: Unsupported format for default pixel data generation: " +
        vk::to_string(format));
  }

  vk::DeviceSize imageSize = pixels.size();

  return createTexture(vulkanDevice, pixels.data(), imageSize, extent, format, transferQueue, false,
                       {}, {}, 1, vk::ImageViewType::e2D, pCustomSamplerInfo);
}
