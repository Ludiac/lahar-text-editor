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
  // --- Public Types ---
  enum class FrameStatus { Success, ResizeNeeded, Error };

  struct SwapchainFrame {
    vk::Image backbuffer{nullptr};
    vk::raii::ImageView backbufferView{nullptr};
    vk::raii::Framebuffer framebuffer{nullptr};
    // New: Semaphore to signal that rendering for this specific swapchain image is complete
    vk::raii::Semaphore presentCompleteSemaphore{nullptr};
  };

  struct PerFrame {
    vk::raii::CommandPool commandPool{nullptr};
    vk::raii::CommandBuffer commandBuffer{nullptr};
    vk::raii::Fence fence{nullptr};
    vk::raii::Semaphore imageAcquiredSemaphore{nullptr};
    // renderCompleteSemaphore is no longer needed here as presentCompleteSemaphore
    // will be used per swapchain image.
    // vk::raii::Semaphore renderCompleteSemaphore{nullptr};
  };

  // --- Public Interface ---
  VulkanWindow(VulkanDevice &device, vk::raii::SurfaceKHR &&surfaceE, bool use_dynamic_renderingg)
      : device_{&device}, surface_{std::move(surfaceE)},
        useDynamicRendering_{use_dynamic_renderingg} {

    if (!*surface_) {
      std::println("VulkanWindow(): invalid surface handle");
      std::exit(1);
    }
    const vk::Format requested_formats[] = {vk::Format::eB8G8R8A8Unorm, vk::Format::eR8G8B8A8Unorm,
                                            vk::Format::eB8G8R8Unorm, vk::Format::eR8G8B8Unorm};
    const vk::PresentModeKHR requested_present_modes[] = {vk::PresentModeKHR::eFifo};

    surfaceFormat_ = selectSurfaceFormat(device_->physical(), requested_formats,
                                         vk::ColorSpaceKHR::eSrgbNonlinear);
    presentMode_ = selectPresentMode(device_->physical(), requested_present_modes);
  }
  VulkanWindow() = default;

  VulkanWindow(const VulkanWindow &) = delete;
  VulkanWindow &operator=(const VulkanWindow &) = delete;
  VulkanWindow(VulkanWindow &&) = default;
  VulkanWindow &operator=(VulkanWindow &&other) = default;

  ~VulkanWindow() {
    if (device_ && *device_->logical()) {
      device_->logical().waitIdle();
    }
  }

  [[nodiscard]] std::expected<void, std::string> createOrResize(vk::Extent2D extent,
                                                                u32 min_image_count = 0) {
    if (*device_->logical())
      device_->logical().waitIdle();

    if (auto result = createSwapchain(extent, min_image_count); !result)
      return std::unexpected("Swapchain creation failed: " + result.error());
    if (auto result = createImageViews(); !result)
      return std::unexpected("ImageView creation failed: " + result.error());
    // New: Create semaphores for each swapchain image
    if (auto result = createSwapchainSyncObjects(); !result)
      return std::unexpected("Swapchain sync objects creation failed: " + result.error());

    if (auto result = createDepthResources(); !result)
      return std::unexpected("Depth resources creation failed: " + result.error());
    if (auto result = createRenderPass(); !result)
      return std::unexpected("RenderPass creation failed: " + result.error());
    if (auto result = createFramebuffers(); !result)
      return std::unexpected("Framebuffer creation failed: " + result.error());
    if (firstCreation_) {
      // Renamed createSyncObjects to createPerFrameSyncObjects and call only once
      if (auto result = createPerFrameSyncObjects(); !result)
        return std::unexpected("Per-frame sync objects creation failed: " + result.error());
      firstCreation_ = false;
    }

    return {};
  }

  // --- Frame Management ---

  [[nodiscard]] FrameStatus
  renderFrame(const std::function<void(vk::raii::CommandBuffer &cmd, vk::raii::RenderPass &rp,
                                       vk ::raii::Framebuffer &fb)> &recordCommands,
              std::span<const vk::CommandBuffer> additionalCommandBuffers = {}) {

    // 1. Begin the frame, acquiring an image
    auto beginResult = beginFrameInternal();
    if (beginResult != vk::Result::eSuccess && beginResult != vk::Result::eSuboptimalKHR) {
      if (beginResult == vk::Result::eErrorOutOfDateKHR) {
        return FrameStatus::ResizeNeeded;
      }
      return FrameStatus::Error;
    }

    // 2. Record commands using the provided lambda
    PerFrame &frame = perFrame_[currentFrame_];
    frame.commandBuffer.begin({});
    recordCommands(frame.commandBuffer, renderPass_, swapchainFrames_[imageIndex_].framebuffer);
    frame.commandBuffer.end();

    // 3. Submit the command buffers
    if (auto submitResult = submitFrameInternal(additionalCommandBuffers); !submitResult) {
      // Error already logged in submitFrameInternal
      return FrameStatus::Error;
    }

    // 4. Present the frame
    auto presentResult = presentFrameInternal();
    if (presentResult == vk::Result::eErrorOutOfDateKHR ||
        presentResult == vk::Result::eSuboptimalKHR || resizeNeeded_) {
      resizeNeeded_ = false;
      return FrameStatus::ResizeNeeded;
    }
    if (presentResult != vk::Result::eSuccess) {
      return FrameStatus::Error;
    }

    return FrameStatus::Success;
  }

  // --- Accessors ---
  [[nodiscard]] vk::Extent2D getExtent() const { return swapchainExtent_; }
  [[nodiscard]] vk::raii::RenderPass &getRenderPass() { return renderPass_; }
  [[nodiscard]] u32 getImageCount() const { return static_cast<u32>(swapchainFrames_.size()); }
  [[nodiscard]] u32 getImageIndex() const { return imageIndex_; }
  [[nodiscard]] u32 getCurrentFrame() const { return currentFrame_; }

  vk::ClearValue clearValue{vk::ClearColorValue(std::array<f32, 4>{0.0, 0.0, 0.0, 1.0})};

private:
  // --- Internal Frame Management ---
  [[nodiscard]] vk::Result beginFrameInternal() {
    PerFrame &frame = perFrame_[currentFrame_];
    auto waitResult = device_->logical().waitForFences({*frame.fence}, vk::True, UINT64_MAX);
    if (waitResult != vk::Result::eSuccess) {
      std::println("Failed to wait for fence: {}", vk::to_string(waitResult));
      return waitResult;
    }

    // Acquire the next image, waiting on the imageAcquiredSemaphore for the current frame in flight
    auto [result, index] = swapchain_.acquireNextImage(UINT64_MAX, *frame.imageAcquiredSemaphore);

    if (result == vk::Result::eSuccess || result == vk::Result::eSuboptimalKHR) {
      imageIndex_ = index;
    } else {
      return result;
    }

    device_->logical().resetFences({*frame.fence});
    frame.commandBuffer.reset();
    return result;
  }

  [[nodiscard]] std::expected<void, std::string>
  submitFrameInternal(std::span<const vk::CommandBuffer> additionalCommandBuffers) {
    PerFrame &frame = perFrame_[currentFrame_];

    std::vector<vk::CommandBuffer> allCommandBuffers;
    allCommandBuffers.push_back(*frame.commandBuffer);
    allCommandBuffers.insert(allCommandBuffers.end(), additionalCommandBuffers.begin(),
                             additionalCommandBuffers.end());

    vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    vk::SubmitInfo submitInfo{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &*frame.imageAcquiredSemaphore, // Wait for image acquisition
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = static_cast<u32>(allCommandBuffers.size()),
        .pCommandBuffers = allCommandBuffers.data(),
        .signalSemaphoreCount = 1,
        // Signal the presentCompleteSemaphore for the *current acquired image*
        .pSignalSemaphores = &*swapchainFrames_[imageIndex_].presentCompleteSemaphore};

    device_->queue_.submit({submitInfo}, *frame.fence);
    return {};
  }

  [[nodiscard]] vk::Result presentFrameInternal() {
    // No longer need to access perFrame_ here for the semaphore,
    // as we use the image-specific presentCompleteSemaphore.
    vk::PresentInfoKHR presentInfo{
        .waitSemaphoreCount = 1,
        // Wait on the presentCompleteSemaphore for the *current acquired image*
        .pWaitSemaphores = &*swapchainFrames_[imageIndex_].presentCompleteSemaphore,
        .swapchainCount = 1,
        .pSwapchains = &*swapchain_,
        .pImageIndices = &imageIndex_};

    auto result = device_->queue_.presentKHR(presentInfo);
    currentFrame_ = (currentFrame_ + 1) % MAX_FRAMES_IN_FLIGHT;
    return result;
  }

  // --- Private Resource Creation ---
  vk::SurfaceFormatKHR selectSurfaceFormat(const vk::raii::PhysicalDevice &physical_device,
                                           std::span<const vk::Format> request_formats,
                                           vk::ColorSpaceKHR request_color_space) {
    const auto avail_formats = physical_device.getSurfaceFormatsKHR(*surface_);
    if (avail_formats.empty())
      return {};
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
    const auto avail_modes = physical_device.getSurfacePresentModesKHR(surface_);
    for (const auto &requested : request_modes) {
      for (const auto &available : avail_modes) {
        if (available == requested) {
          return requested;
        }
      }
    }
    return vk::PresentModeKHR::eFifo;
  }

  [[nodiscard]] std::expected<void, std::string> createSwapchain(vk::Extent2D extent,
                                                                 u32 min_image_count) {
    vk::raii::SwapchainKHR old_swapchain = std::move(swapchain_);
    if (min_image_count == 0) {
      min_image_count = getMinImageCountFromPresentMode(presentMode_);
    }

    auto cap = device_->physical().getSurfaceCapabilitiesKHR(*surface_);
    vk::SwapchainCreateInfoKHR createInfo{
        .surface = *surface_,
        .minImageCount = std::clamp(min_image_count, cap.minImageCount,
                                    cap.maxImageCount > 0 ? cap.maxImageCount : 0x7FFFFFFF),
        .imageFormat = surfaceFormat_.format,
        .imageColorSpace = surfaceFormat_.colorSpace,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment,
        .imageSharingMode = vk::SharingMode::eExclusive,
        .preTransform = cap.currentTransform,
        .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque,
        .presentMode = presentMode_,
        .clipped = vk::True,
        .oldSwapchain = *old_swapchain,
    };

    if (cap.currentExtent.width == 0xFFFFFFFF) {
      createInfo.imageExtent = extent;
    } else {
      createInfo.imageExtent = cap.currentExtent;
    }
    swapchainExtent_ = createInfo.imageExtent;

    auto expectedSwapchain = device_->logical().createSwapchainKHR(createInfo);
    if (!expectedSwapchain) {
      return std::unexpected("Swapchain creation failed: " +
                             vk::to_string(expectedSwapchain.error()));
    }
    swapchain_ = std::move(expectedSwapchain.value());

    auto images = swapchain_.getImages();
    // Ensure all existing semaphores are cleared before resizing and re-creating
    for (auto &frame : swapchainFrames_) {
      frame.presentCompleteSemaphore.clear();
    }
    swapchainFrames_.clear();
    swapchainFrames_.resize(images.size());
    for (u32 i = 0; i < swapchainFrames_.size(); ++i) {
      swapchainFrames_[i].backbuffer = images[i];
    }
    return {};
  }

  [[nodiscard]] std::expected<void, std::string> createImageViews() {
    for (auto &frame : swapchainFrames_) {
      frame.backbufferView.clear();
      const vk::ImageViewCreateInfo createInfo = {
          .image = frame.backbuffer,
          .viewType = vk::ImageViewType::e2D,
          .format = surfaceFormat_.format,
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
      frame.backbufferView = std::move(*expected);
    }
    return {};
  }

  // New function to create semaphores for each swapchain image
  [[nodiscard]] std::expected<void, std::string> createSwapchainSyncObjects() {
    for (auto &frame : swapchainFrames_) {
      // Clear existing semaphore if any
      frame.presentCompleteSemaphore.clear();
      if (auto presentSem = device_->logical().createSemaphore({}); presentSem) {
        frame.presentCompleteSemaphore = std::move(*presentSem);
      } else {
        return std::unexpected("Failed to create present semaphore: " +
                               vk::to_string(presentSem.error()));
      }
    }
    return {};
  }

  [[nodiscard]] std::expected<void, std::string> createDepthResources() {
    depthImageView_.clear();
    depthVmaImage_ = {};
    depthFormat_ = findDepthFormat(device_->physical());
    if (depthFormat_ == vk::Format::eUndefined) {
      return std::unexpected("Failed to find suitable depth format.");
    }
    vk::ImageCreateInfo imageCi{
        .imageType = vk::ImageType::e2D,
        .format = depthFormat_,
        .extent = vk::Extent3D{swapchainExtent_.width, swapchainExtent_.height, 1},
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
    if (useDynamicRendering_) {
      renderPass_.clear();
      return {};
    }
    vk::AttachmentDescription colorAttachment{.format = surfaceFormat_.format,
                                              .samples = vk::SampleCountFlagBits::e1,
                                              .loadOp = vk::AttachmentLoadOp::eClear,
                                              .storeOp = vk::AttachmentStoreOp::eStore,
                                              .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
                                              .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
                                              .initialLayout = vk::ImageLayout::eUndefined,
                                              .finalLayout = vk::ImageLayout::ePresentSrcKHR};
    vk::AttachmentDescription depthAttachment{.format = depthFormat_,
                                              .samples = vk::SampleCountFlagBits::e1,
                                              .loadOp = vk::AttachmentLoadOp::eClear,
                                              .storeOp = vk::AttachmentStoreOp::eDontCare,
                                              .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
                                              .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
                                              .initialLayout = vk::ImageLayout::eUndefined,
                                              .finalLayout =
                                                  vk::ImageLayout::eDepthStencilAttachmentOptimal};
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

    renderPass_.clear();
    auto expectedRenderPass = device_->logical().createRenderPass(renderPassInfo);
    if (!expectedRenderPass) {
      return std::unexpected("RenderPass creation failed: " +
                             vk::to_string(expectedRenderPass.error()));
    }
    renderPass_ = std::move(expectedRenderPass.value());
    return {};
  }

  [[nodiscard]] std::expected<void, std::string> createFramebuffers() {
    if (useDynamicRendering_) {
      for (auto &frame : swapchainFrames_)
        frame.framebuffer.clear();
      return {};
    }
    for (auto &frame : swapchainFrames_) {
      frame.framebuffer.clear();
      std::array<vk::ImageView, 2> attachments = {*frame.backbufferView, *depthImageView_};
      const vk::FramebufferCreateInfo createInfo = {.renderPass = *renderPass_,
                                                    .attachmentCount =
                                                        static_cast<u32>(attachments.size()),
                                                    .pAttachments = attachments.data(),
                                                    .width = swapchainExtent_.width,
                                                    .height = swapchainExtent_.height,
                                                    .layers = 1};

      auto expected = device_->logical().createFramebuffer(createInfo);
      if (!expected) {
        return std::unexpected("Framebuffer creation failed: " + vk::to_string(expected.error()));
      }
      frame.framebuffer = std::move(expected.value());
    }
    return {};
  }

  // Renamed from createSyncObjects to createPerFrameSyncObjects
  [[nodiscard]] std::expected<void, std::string> createPerFrameSyncObjects() {
    for (auto &frame : perFrame_) {
      if (auto pool = device_->logical().createCommandPool(
              {.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
               .queueFamilyIndex = device_->queueFamily_});
          pool) {
        frame.commandPool = std::move(*pool);
      } else {
        return std::unexpected("Failed to create command pool: " + vk::to_string(pool.error()));
      }
      if (auto buffers =
              device_->logical().allocateCommandBuffers({.commandPool = *frame.commandPool,
                                                         .level = vk::CommandBufferLevel::ePrimary,
                                                         .commandBufferCount = 1});
          buffers) {
        frame.commandBuffer = std::move(buffers->front());
      } else {
        return std::unexpected("Failed to allocate command buffer: " +
                               vk::to_string(buffers.error()));
      }
      if (auto fence =
              device_->logical().createFence({.flags = vk::FenceCreateFlagBits::eSignaled});
          fence) {
        frame.fence = std::move(*fence);
      } else {
        return std::unexpected("Failed to create fence: " + vk::to_string(fence.error()));
      }
      if (auto imageSem = device_->logical().createSemaphore({}); imageSem) {
        frame.imageAcquiredSemaphore = std::move(*imageSem);
      } else {
        return std::unexpected("Failed to create image semaphore: " +
                               vk::to_string(imageSem.error()));
      }
      // The renderCompleteSemaphore is removed from PerFrame as it's now per-swapchain image.
      // if (auto renderSem = device_->logical().createSemaphore({}); renderSem) {
      //   frame.renderCompleteSemaphore = std::move(*renderSem);
      // } else {
      //   return std::unexpected("Failed to create render semaphore: " +
      //                          vk::to_string(renderSem.error()));
      // }
    }
    return {};
  }

  // --- Private Members ---
  VulkanDevice *device_;
  vk::raii::SurfaceKHR surface_{nullptr};
  bool firstCreation_{true};
  bool resizeNeeded_{false};

  vk::raii::SwapchainKHR swapchain_{nullptr};
  std::vector<SwapchainFrame> swapchainFrames_;
  vk::Extent2D swapchainExtent_{};
  vk::SurfaceFormatKHR surfaceFormat_{};
  vk::PresentModeKHR presentMode_{};
  bool useDynamicRendering_{};

  VmaImage depthVmaImage_;
  vk::raii::ImageView depthImageView_{nullptr};
  vk::Format depthFormat_{};

  vk::raii::RenderPass renderPass_{nullptr};

  static constexpr u32 MAX_FRAMES_IN_FLIGHT = 2;
  std::array<PerFrame, MAX_FRAMES_IN_FLIGHT> perFrame_;
  u32 currentFrame_{0};
  u32 imageIndex_{0};
};
