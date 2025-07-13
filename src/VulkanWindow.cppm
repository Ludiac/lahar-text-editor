module;

#include "macros.hpp"
#include "primitive_types.hpp"

export module vulkan_app:VulkanWindow;

import vulkan_hpp;
import :VMA;
import :VulkanDevice;
import std;
import :utils;

namespace {
// Anonymous namespace for internal helper functions.

vk::Format findSupportedFormat(const vk::raii::PhysicalDevice &physicalDevice,
                               const std::vector<vk::Format> &candidates, vk::ImageTiling tiling,
                               vk::FormatFeatureFlags features) {
  for (vk::Format format : candidates) {
    vk::FormatProperties props = physicalDevice.getFormatProperties(format);
    if (tiling == vk::ImageTiling::eLinear && (props.linearTilingFeatures & features) == features) {
      return format;
    }
    if (tiling == vk::ImageTiling::eOptimal &&
        (props.optimalTilingFeatures & features) == features) {
      return format;
    }
  }
  return vk::Format::eUndefined;
}

vk::Format findDepthFormat(const vk::raii::PhysicalDevice &physicalDevice) {
  return findSupportedFormat(
      physicalDevice,
      {vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint},
      vk::ImageTiling::eOptimal, vk::FormatFeatureFlagBits::eDepthStencilAttachment);
}

u32 getMinImageCountFromPresentMode(vk::PresentModeKHR present_mode) {
  if (present_mode == vk::PresentModeKHR::eMailbox) {
    return 3;
  }
  if (present_mode == vk::PresentModeKHR::eFifo ||
      present_mode == vk::PresentModeKHR::eFifoRelaxed) {
    return 2;
  }
  if (present_mode == vk::PresentModeKHR::eImmediate) {
    return 1;
  }
  return 1;
}

} // namespace

export class VulkanWindow {
public:
  // Publicly accessible frame data
  struct Frame {
    vk::raii::CommandPool CommandPool{nullptr};
    vk::raii::CommandBuffer CommandBuffer{nullptr};
    vk::raii::Fence Fence{nullptr};
    vk::Image Backbuffer{nullptr}; // Not RAII as it's owned by the swapchain
    vk::raii::ImageView BackbufferView{nullptr};
    vk::raii::Framebuffer Framebuffer{nullptr};
  };

  struct FrameSemaphores {
    vk::raii::Semaphore ImageAcquiredSemaphore{nullptr};
    vk::raii::Semaphore RenderCompleteSemaphore{nullptr};
  };

  // --- Public Interface ---
  VulkanWindow(VulkanDevice &device, vk::raii::SurfaceKHR &&surfaceE, bool use_dynamic_renderingg)
      : device_{&device}, surface{std::move(surfaceE)},
        useDynamicRendering{use_dynamic_renderingg} {

    if (!*surface) {
      std::println("VulkanWindow(): invalid surface handle");
      std::exit(1);
    }
    const vk::Format requested_formats[] = {vk::Format::eB8G8R8A8Unorm, vk::Format::eR8G8B8A8Unorm,
                                            vk::Format::eB8G8R8Unorm, vk::Format::eR8G8B8Unorm};
    const vk::PresentModeKHR requested_present_modes[] = {
        // vk::PresentModeKHR::eMailbox, vk::PresentModeKHR::eFifo, vk::PresentModeKHR::eImmediate};
        vk::PresentModeKHR::eFifo};

    surfaceFormat = selectSurfaceFormat(device_->physical(), requested_formats,
                                        vk::ColorSpaceKHR::eSrgbNonlinear);
    presentMode = selectPresentMode(device_->physical(), requested_present_modes);
  }
  VulkanWindow() = default;

  VulkanWindow(const VulkanWindow &) = delete;
  VulkanWindow &operator=(const VulkanWindow &) = delete;
  VulkanWindow(VulkanWindow &&) = default;
  VulkanWindow &operator=(VulkanWindow &&other) = default;

  ~VulkanWindow() {
    // RAII handles most cleanup. VmaImage has its own destructor.
    // Ensure device is idle before we start tearing things down.
    if (*device_->logical()) {
      device_->logical().waitIdle();
    }
  }

  // Main function to create or recreate the swapchain and all dependent resources.
  [[nodiscard]] std::expected<void, std::string> createOrResize(vk::Extent2D extent,
                                                                u32 min_image_count = 0) {
    // Wait for the device to be idle before messing with resources.
    device_->logical().waitIdle();

    // Re-create all resources.
    // The order of operations is important.
    if (auto result = createSwapchain(extent, min_image_count); !result)
      return std::unexpected("Swapchain creation failed: " + result.error());
    if (auto result = createImageViews(); !result)
      return std::unexpected("ImageView creation failed: " + result.error());
    if (auto result = createDepthResources(); !result)
      return std::unexpected("Depth resources creation failed: " + result.error());
    if (auto result = createRenderPass(); !result)
      return std::unexpected("RenderPass creation failed: " + result.error());
    if (auto result = createFramebuffers(); !result)
      return std::unexpected("Framebuffer creation failed: " + result.error());
    if (auto result = createFrameSpecificResources(); !result)
      return std::unexpected("Frame-specific resources creation failed: " + result.error());

    return {};
  }

  // const vk::raii::SwapchainKHR &getSwapchain() const { return swapchain_; }
  // const vk::raii::RenderPass &getRenderPass() const { return renderPass_; }
  // const Frame &getCurrentFrame() const { return frames_[frameIndex_]; }
  // Frame &getCurrentFrame() { return frames_[frameIndex_]; }
  // const FrameSemaphores &getCurrentSemaphores() const { return frameSemaphores_[semaphoreIndex_];
  // } vk::Extent2D getExtent() const { return swapchainExtent_; } vk::Format getSurfaceFormat()
  // const { return surfaceFormat_.format; } vk::Format getDepthFormat() const { return
  // depthFormat_; }
private:
  vk::SurfaceFormatKHR selectSurfaceFormat(const vk::raii::PhysicalDevice &physical_device,
                                           std::span<const vk::Format> request_formats,
                                           vk::ColorSpaceKHR request_color_space) {
    const auto avail_formats = physical_device.getSurfaceFormatsKHR(*surface);

    if (avail_formats.empty()) {
      return {};
    }

    if (avail_formats.size() == 1 && avail_formats[0].format == vk::Format::eUndefined) {
      return vk::SurfaceFormatKHR{.format = request_formats.front(),
                                  .colorSpace = request_color_space};
    }

    for (const auto &requested : request_formats) {
      for (const auto &available : avail_formats) {
        if (available.format == requested && available.colorSpace == request_color_space) {
          return available;
        }
      }
    }

    return avail_formats.front();
  }

  vk::PresentModeKHR selectPresentMode(const vk::raii::PhysicalDevice &physical_device,
                                       std::span<const vk::PresentModeKHR> request_modes) {
    const auto avail_modes = physical_device.getSurfacePresentModesKHR(surface);

    for (const auto &requested : request_modes) {
      for (const auto &available : avail_modes) {
        if (available == requested) {
          return requested;
        }
      }
    }

    return vk::PresentModeKHR::eFifo; // Guaranteed to be available.
  }

  // --- Private Methods ---
  [[nodiscard]] std::expected<void, std::string> createSwapchain(vk::Extent2D extent,
                                                                 u32 min_image_count) {
    vk::raii::SwapchainKHR old_swapchain = std::move(swapchain);

    if (min_image_count == 0) {
      min_image_count = getMinImageCountFromPresentMode(presentMode);
    }

    auto cap = device_->physical().getSurfaceCapabilitiesKHR(*surface);

    vk::SwapchainCreateInfoKHR createInfo{
        .surface = *surface,
        .minImageCount = std::clamp(min_image_count, cap.minImageCount,
                                    cap.maxImageCount > 0 ? cap.maxImageCount : 0x7FFFFFFF),
        .imageFormat = surfaceFormat.format,
        .imageColorSpace = surfaceFormat.colorSpace,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment,
        .imageSharingMode = vk::SharingMode::eExclusive,
        .preTransform = cap.currentTransform,
        .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque,
        .presentMode = presentMode,
        .clipped = vk::True,
        .oldSwapchain = *old_swapchain,
    };

    if (cap.currentExtent.width == 0xFFFFFFFF) {
      createInfo.imageExtent = extent;
    } else {
      createInfo.imageExtent = cap.currentExtent;
    }
    swapchainExtent = createInfo.imageExtent;

    auto expectedSwapchain = device_->logical().createSwapchainKHR(createInfo);
    if (!expectedSwapchain) {
      return std::unexpected("Swapchain creation failed: " +
                             vk::to_string(expectedSwapchain.error()));
    }
    swapchain = std::move(expectedSwapchain.value());

    auto images = swapchain.getImages();
    frames.clear(); // Clear old frame data
    frames.resize(images.size());
    for (u32 i = 0; i < frames.size(); ++i) {
      frames[i].Backbuffer = images[i];
    }

    return {};
  }

  [[nodiscard]] std::expected<void, std::string> createImageViews() {
    for (auto &frame : frames) {
      frame.BackbufferView.clear(); // Clear old one if it exists

      const vk::ImageViewCreateInfo createInfo = {
          .image = frame.Backbuffer,
          .viewType = vk::ImageViewType::e2D,
          .format = surfaceFormat.format,
          .components = {.r = vk::ComponentSwizzle::eR,
                         .g = vk::ComponentSwizzle::eG,
                         .b = vk::ComponentSwizzle::eB,
                         .a = vk::ComponentSwizzle::eA},
          .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                               .baseMipLevel = 0,
                               .levelCount = 1,
                               .baseArrayLayer = 0,
                               .layerCount = 1}};

      auto expected = device_->logical().createImageView(createInfo);
      if (!expected) {
        return std::unexpected("ImageView creation failed: " + vk::to_string(expected.error()));
      }
      frame.BackbufferView = std::move(*expected);
    }
    return {};
  }

  [[nodiscard]] std::expected<void, std::string> createDepthResources() {
    depthImageView_.clear();
    depthVmaImage_ = {}; // Let VMA handle destruction of the old image

    depthFormat_ = findDepthFormat(device_->physical());
    if (depthFormat_ == vk::Format::eUndefined) {
      return std::unexpected("Failed to find suitable depth format.");
    }

    vk::ImageCreateInfo imageCi{.imageType = vk::ImageType::e2D,
                                .format = depthFormat_,
                                .extent =
                                    vk::Extent3D{swapchainExtent.width, swapchainExtent.height, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = vk::SampleCountFlagBits::e1,
                                .tiling = vk::ImageTiling::eOptimal,
                                .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment,
                                .sharingMode = vk::SharingMode::eExclusive,
                                .initialLayout = vk::ImageLayout::eUndefined};

    vma::AllocationCreateInfo imageAllocInfo{.usage = vma::MemoryUsage::eAutoPreferDevice};

    auto vmaImageResult = device_->createImageVMA(imageCi, imageAllocInfo);
    if (!vmaImageResult) {
      return std::unexpected("VMA depth image creation failed: " + vmaImageResult.error());
    }
    depthVmaImage_ = std::move(vmaImageResult.value());

    vk::ImageSubresourceRange depthSubresourceRange{.aspectMask = vk::ImageAspectFlagBits::eDepth,
                                                    .baseMipLevel = 0,
                                                    .levelCount = 1,
                                                    .baseArrayLayer = 0,
                                                    .layerCount = 1};
    if (depthFormat_ == vk::Format::eD32SfloatS8Uint ||
        depthFormat_ == vk::Format::eD24UnormS8Uint) {
      depthSubresourceRange.aspectMask |= vk::ImageAspectFlagBits::eStencil;
    }

    vk::ImageViewCreateInfo viewInfo{.image = depthVmaImage_.get(),
                                     .viewType = vk::ImageViewType::e2D,
                                     .format = depthVmaImage_.getFormat(),
                                     .subresourceRange = depthSubresourceRange};

    auto viewResult = device_->logical().createImageView(viewInfo);
    if (!viewResult) {
      return std::unexpected("Failed to create depth image view: " +
                             vk::to_string(viewResult.error()));
    }
    depthImageView_ = std::move(viewResult.value());

    // Transition image layout
    return device_->executeSingleTimeCommands([&](vk::CommandBuffer cmdBuffer) {
      vk::ImageMemoryBarrier barrier{.srcAccessMask = vk::AccessFlagBits::eNone,
                                     .dstAccessMask =
                                         vk::AccessFlagBits::eDepthStencilAttachmentRead |
                                         vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                                     .oldLayout = vk::ImageLayout::eUndefined,
                                     .newLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
                                     .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                                     .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                                     .image = depthVmaImage_.get(),
                                     .subresourceRange = depthSubresourceRange};
      cmdBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                vk::PipelineStageFlagBits::eEarlyFragmentTests |
                                    vk::PipelineStageFlagBits::eLateFragmentTests,
                                {}, nullptr, nullptr, {barrier});
    });
  }

  [[nodiscard]] std::expected<void, std::string> createRenderPass() {
    if (useDynamicRendering) {
      renderPass.clear(); // Not used in dynamic rendering
      return {};
    }

    vk::AttachmentDescription colorAttachment{
        .format = surfaceFormat.format,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore, // Store for presentation
        .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
        .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
        .initialLayout = vk::ImageLayout::eUndefined,
        .finalLayout = vk::ImageLayout::ePresentSrcKHR};

    vk::AttachmentDescription depthAttachment{
        .format = depthFormat_,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eDontCare, // Not needed after render
        .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
        .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
        .initialLayout = vk::ImageLayout::eUndefined,
        .finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal};

    std::array<vk::AttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};

    vk::AttachmentReference colorAttachmentRef{.attachment = 0,
                                               .layout = vk::ImageLayout::eColorAttachmentOptimal};
    vk::AttachmentReference depthAttachmentRef{
        .attachment = 1, .layout = vk::ImageLayout::eDepthStencilAttachmentOptimal};

    vk::SubpassDescription subpass{.pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
                                   .colorAttachmentCount = 1,
                                   .pColorAttachments = &colorAttachmentRef,
                                   .pDepthStencilAttachment = &depthAttachmentRef};

    vk::SubpassDependency dependency{
        .srcSubpass = vk::SubpassExternal,
        .dstSubpass = 0,
        .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                        vk::PipelineStageFlagBits::eEarlyFragmentTests,
        .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                        vk::PipelineStageFlagBits::eEarlyFragmentTests,
        .srcAccessMask = vk::AccessFlagBits::eNone,
        .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite |
                         vk::AccessFlagBits::eDepthStencilAttachmentWrite};

    vk::RenderPassCreateInfo renderPassInfo{.attachmentCount = static_cast<u32>(attachments.size()),
                                            .pAttachments = attachments.data(),
                                            .subpassCount = 1,
                                            .pSubpasses = &subpass,
                                            .dependencyCount = 1,
                                            .pDependencies = &dependency};

    renderPass.clear();
    auto expectedRenderPass = device_->logical().createRenderPass(renderPassInfo);
    if (!expectedRenderPass) {
      return std::unexpected("RenderPass creation failed: " +
                             vk::to_string(expectedRenderPass.error()));
    }
    renderPass = std::move(expectedRenderPass.value());
    return {};
  }

  [[nodiscard]] std::expected<void, std::string> createFramebuffers() {
    if (useDynamicRendering) {
      for (auto &frame : frames)
        frame.Framebuffer.clear(); // Not used
      return {};
    }

    for (auto &frame : frames) {
      frame.Framebuffer.clear();
      std::array<vk::ImageView, 2> attachments = {*frame.BackbufferView, *depthImageView_};

      const vk::FramebufferCreateInfo createInfo = {.renderPass = *renderPass,
                                                    .attachmentCount =
                                                        static_cast<u32>(attachments.size()),
                                                    .pAttachments = attachments.data(),
                                                    .width = swapchainExtent.width,
                                                    .height = swapchainExtent.height,
                                                    .layers = 1};

      auto expected = device_->logical().createFramebuffer(createInfo);
      if (!expected) {
        return std::unexpected("Framebuffer creation failed: " + vk::to_string(expected.error()));
      }
      frame.Framebuffer = std::move(expected.value());
    }
    return {};
  }

  [[nodiscard]] std::expected<void, std::string> createFrameSpecificResources() {
    // Create command pools, buffers, and fences for each frame in flight
    for (auto &frame : frames) {
      frame.CommandPool.clear();
      frame.CommandBuffer.clear();
      frame.Fence.clear();

      if (auto pool = device_->logical().createCommandPool({
              .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
              .queueFamilyIndex = device_->queueFamily_,
          });
          pool) {
        frame.CommandPool = std::move(*pool);
      } else {
        return std::unexpected("Failed to create command pool: " + vk::to_string(pool.error()));
      }

      if (auto buffers = device_->logical().allocateCommandBuffers({
              .commandPool = *frame.CommandPool,
              .level = vk::CommandBufferLevel::ePrimary,
              .commandBufferCount = 1,
          });
          buffers) {
        frame.CommandBuffer = std::move(buffers->front());
      } else {
        return std::unexpected("Failed to allocate command buffer: " +
                               vk::to_string(buffers.error()));
      }

      if (auto fence =
              device_->logical().createFence({.flags = vk::FenceCreateFlagBits::eSignaled});
          fence) {
        frame.Fence = std::move(*fence);
      } else {
        return std::unexpected("Failed to create fence: " + vk::to_string(fence.error()));
      }
    }

    // Create semaphores
    frameSemaphores.clear();
    frameSemaphores.resize(frames.size() + 1);
    for (auto &semaphores : frameSemaphores) {
      if (auto imageSem = device_->logical().createSemaphore({}); imageSem) {
        semaphores.ImageAcquiredSemaphore = std::move(*imageSem);
      } else {
        return std::unexpected("Failed to create image semaphore: " +
                               vk::to_string(imageSem.error()));
      }

      if (auto renderSem = device_->logical().createSemaphore({}); renderSem) {
        semaphores.RenderCompleteSemaphore = std::move(*renderSem);
      } else {
        return std::unexpected("Failed to create render semaphore: " +
                               vk::to_string(renderSem.error()));
      }
    }

    return {};
  }

  // --- Private Members ---
  VulkanDevice *device_;

  vk::SurfaceFormatKHR surfaceFormat{};
  // Depth buffer resources
  VmaImage depthVmaImage_;
  vk::raii::ImageView depthImageView_{nullptr};
  vk::Format depthFormat_{};

public:
  // Frame-specific resources
  std::vector<Frame> frames;
  std::vector<FrameSemaphores> frameSemaphores;

  // Window-specific resources
  vk::raii::SurfaceKHR surface{nullptr};
  vk::raii::SwapchainKHR swapchain{nullptr};
  vk::raii::RenderPass renderPass{nullptr};

  // Configuration
  vk::Extent2D swapchainExtent{};
  vk::PresentModeKHR presentMode{};
  bool useDynamicRendering{};

  [[nodiscard]] usize getImageCount() const { return frames.size(); }

  // --- Public Members ---
  u32 frameIndex{};
  u32 semaphoreIndex{};
  vk::ClearValue clearValue{vk::ClearColorValue(std::array<f32, 4>{0.0, 0.0, 0.0, 1.0})};
};
