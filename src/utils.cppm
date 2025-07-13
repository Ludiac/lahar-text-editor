module;

#include "primitive_types.hpp"
// #include <glm/fwd.hpp>
// #include <glm/glm.hpp>
// #include <glm/gtc/matrix_transform.hpp>
// // #include <glm/gtc/quaternion.hpp>

export module vulkan_app:utils;

import std;
import vulkan_hpp;
import :Types;

export [[nodiscard]] std::expected<u32, std::string>
findMemoryType(const vk::raii::PhysicalDevice &physicalDevice, u32 typeFilter,
               vk::MemoryPropertyFlags properties) {
  vk::PhysicalDeviceMemoryProperties memProperties = physicalDevice.getMemoryProperties();
  for (u32 i = 0; i < memProperties.memoryTypeCount; ++i) {
    if (((typeFilter & (1 << i)) != 0) &&
        (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
      return i;
    }
  }
  return std::unexpected("TextureHelpers::findMemoryType: Failed to find suitable memory type.");
}

void transitionImageLayout(const vk::raii::CommandBuffer &commandBuffer, vk::Image image,
                           vk::Format format, vk::ImageLayout oldLayout, vk::ImageLayout newLayout,
                           const vk::ImageSubresourceRange &subresourceRange) {
  vk::ImageMemoryBarrier barrier{.srcAccessMask = {},
                                 .dstAccessMask = {},
                                 .oldLayout = oldLayout,
                                 .newLayout = newLayout,
                                 .srcQueueFamilyIndex = 0,
                                 .dstQueueFamilyIndex = 0,
                                 .image = image,
                                 .subresourceRange = subresourceRange};

  vk::PipelineStageFlags sourceStage;
  vk::PipelineStageFlags destinationStage;

  if (oldLayout == vk::ImageLayout::eUndefined &&
      newLayout == vk::ImageLayout::eTransferDstOptimal) {
    barrier.srcAccessMask = {};
    barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
    sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
    destinationStage = vk::PipelineStageFlagBits::eTransfer;
  } else if (oldLayout == vk::ImageLayout::eTransferDstOptimal &&
             newLayout == vk::ImageLayout::eShaderReadOnlyOptimal) {
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
    sourceStage = vk::PipelineStageFlagBits::eTransfer;
    destinationStage = vk::PipelineStageFlagBits::eFragmentShader;
  } else if (oldLayout == vk::ImageLayout::eTransferDstOptimal &&
             newLayout == vk::ImageLayout::eTransferSrcOptimal) {
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
    sourceStage = vk::PipelineStageFlagBits::eTransfer;
    destinationStage = vk::PipelineStageFlagBits::eTransfer;
  } else if (oldLayout == vk::ImageLayout::eTransferSrcOptimal &&
             newLayout == vk::ImageLayout::eShaderReadOnlyOptimal) {
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
    sourceStage = vk::PipelineStageFlagBits::eTransfer;
    destinationStage = vk::PipelineStageFlagBits::eFragmentShader;
  } else if (oldLayout == vk::ImageLayout::eUndefined && newLayout == vk::ImageLayout::eGeneral) {
    barrier.srcAccessMask = {};
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
    sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
    destinationStage = vk::PipelineStageFlagBits::eComputeShader;
  } else if (oldLayout == vk::ImageLayout::eUndefined &&
             (newLayout == vk::ImageLayout::eColorAttachmentOptimal ||
              newLayout == vk::ImageLayout::eDepthStencilAttachmentOptimal)) {
    barrier.srcAccessMask = {};
    barrier.dstAccessMask = (newLayout == vk::ImageLayout::eColorAttachmentOptimal)
                                ? vk::AccessFlagBits::eColorAttachmentWrite
                                : vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
    destinationStage = (newLayout == vk::ImageLayout::eColorAttachmentOptimal)
                           ? vk::PipelineStageFlagBits::eColorAttachmentOutput
                           : vk::PipelineStageFlagBits::eEarlyFragmentTests;
  } else {
    barrier.srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eMemoryRead;
    barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
    sourceStage = vk::PipelineStageFlagBits::eAllCommands;
    destinationStage = vk::PipelineStageFlagBits::eAllCommands;
  }

  // This check might be redundant if subresourceRange.aspectMask is always correctly set by the
  // caller.
  if (barrier.subresourceRange.aspectMask == vk::ImageAspectFlags{}) {
    if (newLayout == vk::ImageLayout::eDepthStencilAttachmentOptimal ||
        oldLayout == vk::ImageLayout::eDepthStencilAttachmentOptimal ||
        format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint ||
        format == vk::Format::eD16UnormS8Uint || format == vk::Format::eD32Sfloat ||
        format == vk::Format::eD16Unorm) {
      barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
      if (format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint ||
          format == vk::Format::eD16UnormS8Uint) {
        barrier.subresourceRange.aspectMask |= vk::ImageAspectFlagBits::eStencil;
      }
    } else {
      barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    }
  }

  commandBuffer.pipelineBarrier(sourceStage, destinationStage, {}, nullptr, nullptr, barrier);
}
