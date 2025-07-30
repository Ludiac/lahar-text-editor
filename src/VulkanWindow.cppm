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

u32 getMinImageCountFromPresentMode(vk::PresentModeKHR presentMode) {
  if (presentMode == vk::PresentModeKHR::eMailbox) {
    return 3;
  }
  if (presentMode == vk::PresentModeKHR::eFifo || presentMode == vk::PresentModeKHR::eFifoRelaxed) {
    return 2;
  }
  if (presentMode == vk::PresentModeKHR::eImmediate) {
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
    vk::raii::Semaphore presentCompleteSemaphore{nullptr};
  };

  struct PerFrame {
    vk::raii::CommandPool commandPool{nullptr};
    vk::raii::CommandBuffer commandBuffer{nullptr};
    vk::raii::Fence fence{nullptr};
    vk::raii::Semaphore imageAcquiredSemaphore{nullptr};
  };

  // --- Public Interface ---
  VulkanWindow(VulkanDevice &device, vk::raii::SurfaceKHR &&surfaceE, bool useDynamicRenderingg)
      : m_device{&device}, m_surface{std::move(surfaceE)},
        m_useDynamicRendering{useDynamicRenderingg} {

    if (!*m_surface) {
      std::println("VulkanWindow(): invalid surface handle");
      std::exit(1);
    }
    const vk::Format REQUESTED_FORMATS[] = {vk::Format::eB8G8R8A8Unorm, vk::Format::eR8G8B8A8Unorm,
                                            vk::Format::eB8G8R8Unorm, vk::Format::eR8G8B8Unorm};
    const vk::PresentModeKHR REQUESTED_PRESENT_MODES[] = {vk::PresentModeKHR::eFifo};

    m_surfaceFormat = selectSurfaceFormat(m_device->physical(), REQUESTED_FORMATS,
                                          vk::ColorSpaceKHR::eSrgbNonlinear);
    m_presentMode = selectPresentMode(m_device->physical(), REQUESTED_PRESENT_MODES);
  }
  VulkanWindow() = default;

  VulkanWindow(const VulkanWindow &) = delete;
  VulkanWindow &operator=(const VulkanWindow &) = delete;
  VulkanWindow(VulkanWindow &&) = default;
  VulkanWindow &operator=(VulkanWindow &&other) = default;

  ~VulkanWindow() {
    if ((m_device != nullptr) && *m_device->logical()) {
      m_device->logical().waitIdle();
    }
  }

  [[nodiscard]] std::expected<void, std::string> createOrResize(vk::Extent2D extent,
                                                                u32 minImageCount = 0) {
    if (*m_device->logical()) {
      m_device->logical().waitIdle();
    }

    if (auto result = createSwapchain(extent, minImageCount); !result) {
      return std::unexpected("Swapchain creation failed: " + result.error());
    }
    if (auto result = createImageViews(); !result) {
      return std::unexpected("ImageView creation failed: " + result.error());
    }
    if (auto result = createSwapchainSyncObjects(); !result) {
      return std::unexpected("Swapchain sync objects creation failed: " + result.error());
    }

    if (auto result = createDepthResources(); !result) {
      return std::unexpected("Depth resources creation failed: " + result.error());
    }
    if (auto result = createRenderPass(); !result) {
      return std::unexpected("RenderPass creation failed: " + result.error());
    }
    if (auto result = createFramebuffers(); !result) {
      return std::unexpected("Framebuffer creation failed: " + result.error());
    }
    if (m_firstCreation) {
      if (auto result = createPerFrameSyncObjects(); !result) {
        return std::unexpected("Per-frame sync objects creation failed: " + result.error());
      }
      m_firstCreation = false;
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
    PerFrame &frame = m_perFrame[m_currentFrame];
    frame.commandBuffer.begin({});
    recordCommands(frame.commandBuffer, m_renderPass, m_swapchainFrames[m_imageIndex].framebuffer);
    frame.commandBuffer.end();

    // 3. Submit the command buffers
    if (auto submitResult = submitFrameInternal(additionalCommandBuffers); !submitResult) {
      // Error already logged in submitFrameInternal
      return FrameStatus::Error;
    }

    // 4. Present the frame
    auto presentResult = presentFrameInternal();
    if (presentResult == vk::Result::eErrorOutOfDateKHR ||
        presentResult == vk::Result::eSuboptimalKHR || m_resizeNeeded) {
      m_resizeNeeded = false;
      return FrameStatus::ResizeNeeded;
    }
    if (presentResult != vk::Result::eSuccess) {
      return FrameStatus::Error;
    }

    return FrameStatus::Success;
  }

  // --- Accessors ---
  [[nodiscard]] vk::Extent2D extent() const { return m_swapchainExtent; }
  [[nodiscard]] const vk::raii::RenderPass &renderPass() { return m_renderPass; }
  [[nodiscard]] vk::ClearValue clearValue() const { return m_clearValue; }
  [[nodiscard]] u32 imageCount() const { return static_cast<u32>(m_swapchainFrames.size()); }
  [[nodiscard]] u32 imageIndex() const { return m_imageIndex; }
  [[nodiscard]] u32 currentFrame() const { return m_currentFrame; }

private:
  // --- Internal Frame Management ---
  [[nodiscard]] vk::Result beginFrameInternal() {
    PerFrame &frame = m_perFrame[m_currentFrame];
    auto waitResult = m_device->logical().waitForFences({*frame.fence}, vk::True, UINT64_MAX);
    if (waitResult != vk::Result::eSuccess) {
      std::println("Failed to wait for fence: {}", vk::to_string(waitResult));
      return waitResult;
    }

    // Acquire the next image, waiting on the imageAcquiredSemaphore for the current frame in flight
    auto [result, index] = m_swapchain.acquireNextImage(UINT64_MAX, *frame.imageAcquiredSemaphore);

    if (result == vk::Result::eSuccess || result == vk::Result::eSuboptimalKHR) {
      m_imageIndex = index;
    } else {
      return result;
    }

    m_device->logical().resetFences({*frame.fence});
    frame.commandBuffer.reset();
    return result;
  }

  [[nodiscard]] std::expected<void, std::string>
  submitFrameInternal(std::span<const vk::CommandBuffer> additionalCommandBuffers) {
    PerFrame &frame = m_perFrame[m_currentFrame];

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
        .pSignalSemaphores = &*m_swapchainFrames[m_imageIndex].presentCompleteSemaphore};

    m_device->graphicsQueue()->submit({submitInfo}, *frame.fence);
    return {};
  }

  [[nodiscard]] vk::Result presentFrameInternal() {
    // No longer need to access perFrame_ here for the semaphore,
    // as we use the image-specific presentCompleteSemaphore.
    vk::PresentInfoKHR presentInfo{
        .waitSemaphoreCount = 1,
        // Wait on the presentCompleteSemaphore for the *current acquired image*
        .pWaitSemaphores = &*m_swapchainFrames[m_imageIndex].presentCompleteSemaphore,
        .swapchainCount = 1,
        .pSwapchains = &*m_swapchain,
        .pImageIndices = &m_imageIndex};

    auto result = m_device->graphicsQueue()->presentKHR(presentInfo);
    m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    return result;
  }

  // --- Private Resource Creation ---
  vk::SurfaceFormatKHR selectSurfaceFormat(const vk::raii::PhysicalDevice &physicalDevice,
                                           std::span<const vk::Format> requestFormats,
                                           vk::ColorSpaceKHR requestColorSpace) {
    const auto AVAIL_FORMATS = physicalDevice.getSurfaceFormatsKHR(*m_surface);
    if (AVAIL_FORMATS.empty()) {
      return {};
    }
    if (AVAIL_FORMATS.size() == 1 && AVAIL_FORMATS[0].format == vk::Format::eUndefined) {
      return vk::SurfaceFormatKHR{.format = requestFormats.front(),
                                  .colorSpace = requestColorSpace};
    }
    for (const auto &requested : requestFormats) {
      for (const auto &available : AVAIL_FORMATS) {
        if (available.format == requested && available.colorSpace == requestColorSpace) {
          return available;
        }
      }
    }
    return AVAIL_FORMATS.front();
  }

  vk::PresentModeKHR selectPresentMode(const vk::raii::PhysicalDevice &physicalDevice,
                                       std::span<const vk::PresentModeKHR> requestModes) {
    const auto AVAIL_MODES = physicalDevice.getSurfacePresentModesKHR(m_surface);
    for (const auto &requested : requestModes) {
      for (const auto &available : AVAIL_MODES) {
        if (available == requested) {
          return requested;
        }
      }
    }
    return vk::PresentModeKHR::eFifo;
  }

  [[nodiscard]] std::expected<void, std::string> createSwapchain(vk::Extent2D extent,
                                                                 u32 minImageCount) {
    vk::raii::SwapchainKHR oldSwapchain = std::move(m_swapchain);
    if (minImageCount == 0) {
      minImageCount = getMinImageCountFromPresentMode(m_presentMode);
    }

    auto cap = m_device->physical().getSurfaceCapabilitiesKHR(*m_surface);
    vk::SwapchainCreateInfoKHR createInfo{
        .surface = *m_surface,
        .minImageCount = std::clamp(minImageCount, cap.minImageCount,
                                    cap.maxImageCount > 0 ? cap.maxImageCount : 0x7FFFFFFF),
        .imageFormat = m_surfaceFormat.format,
        .imageColorSpace = m_surfaceFormat.colorSpace,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment,
        .imageSharingMode = vk::SharingMode::eExclusive,
        .preTransform = cap.currentTransform,
        .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque,
        .presentMode = m_presentMode,
        .clipped = vk::True,
        .oldSwapchain = *oldSwapchain,
    };

    if (cap.currentExtent.width == 0xFFFFFFFF) {
      createInfo.imageExtent = extent;
    } else {
      createInfo.imageExtent = cap.currentExtent;
    }
    m_swapchainExtent = createInfo.imageExtent;

    auto expectedSwapchain = m_device->logical().createSwapchainKHR(createInfo);
    if (!expectedSwapchain) {
      return std::unexpected("Swapchain creation failed: " +
                             vk::to_string(expectedSwapchain.error()));
    }
    m_swapchain = std::move(expectedSwapchain.value());

    auto images = m_swapchain.getImages();
    // Ensure all existing semaphores are cleared before resizing and re-creating
    for (auto &frame : m_swapchainFrames) {
      frame.presentCompleteSemaphore.clear();
    }
    m_swapchainFrames.clear();
    m_swapchainFrames.resize(images.size());
    for (u32 i = 0; i < m_swapchainFrames.size(); ++i) {
      m_swapchainFrames[i].backbuffer = images[i];
    }
    return {};
  }

  [[nodiscard]] std::expected<void, std::string> createImageViews() {
    for (auto &frame : m_swapchainFrames) {
      frame.backbufferView.clear();
      const vk::ImageViewCreateInfo CREATE_INFO = {
          .image = frame.backbuffer,
          .viewType = vk::ImageViewType::e2D,
          .format = m_surfaceFormat.format,
          .components = {.r = vk::ComponentSwizzle::eR,
                         .g = vk::ComponentSwizzle::eG,
                         .b = vk::ComponentSwizzle::eB,
                         .a = vk::ComponentSwizzle::eA},
          .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                               .baseMipLevel = 0,
                               .levelCount = 1,
                               .baseArrayLayer = 0,
                               .layerCount = 1}};

      auto expected = m_device->logical().createImageView(CREATE_INFO);
      if (!expected) {
        return std::unexpected("ImageView creation failed: " + vk::to_string(expected.error()));
      }
      frame.backbufferView = std::move(*expected);
    }
    return {};
  }

  // New function to create semaphores for each swapchain image
  [[nodiscard]] std::expected<void, std::string> createSwapchainSyncObjects() {
    for (auto &frame : m_swapchainFrames) {
      // Clear existing semaphore if any
      frame.presentCompleteSemaphore.clear();
      if (auto presentSem = m_device->logical().createSemaphore({}); presentSem) {
        frame.presentCompleteSemaphore = std::move(*presentSem);
      } else {
        return std::unexpected("Failed to create present semaphore: " +
                               vk::to_string(presentSem.error()));
      }
    }
    return {};
  }

  [[nodiscard]] std::expected<void, std::string> createDepthResources() {
    m_depthImageView.clear();
    m_depthVmaImage = {};
    m_depthFormat = findDepthFormat(m_device->physical());
    if (m_depthFormat == vk::Format::eUndefined) {
      return std::unexpected("Failed to find suitable depth format.");
    }
    vk::ImageCreateInfo imageCi{.imageType = vk::ImageType::e2D,
                                .format = m_depthFormat,
                                .extent = vk::Extent3D{.width = m_swapchainExtent.width,
                                                       .height = m_swapchainExtent.height,
                                                       .depth = 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = vk::SampleCountFlagBits::e1,
                                .tiling = vk::ImageTiling::eOptimal,
                                .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment,
                                .sharingMode = vk::SharingMode::eExclusive,
                                .initialLayout = vk::ImageLayout::eUndefined};
    vma::AllocationCreateInfo imageAllocInfo{.usage = vma::MemoryUsage::eAutoPreferDevice};
    auto vmaImageResult = m_device->createImageVMA(imageCi, imageAllocInfo);
    if (!vmaImageResult) {
      return std::unexpected("VMA depth image creation failed: " + vmaImageResult.error());
    }
    m_depthVmaImage = std::move(vmaImageResult.value());
    vk::ImageSubresourceRange depthSubresourceRange{.aspectMask = vk::ImageAspectFlagBits::eDepth,
                                                    .baseMipLevel = 0,
                                                    .levelCount = 1,
                                                    .baseArrayLayer = 0,
                                                    .layerCount = 1};
    if (m_depthFormat == vk::Format::eD32SfloatS8Uint ||
        m_depthFormat == vk::Format::eD24UnormS8Uint) {
      depthSubresourceRange.aspectMask |= vk::ImageAspectFlagBits::eStencil;
    }
    vk::ImageViewCreateInfo viewInfo{.image = m_depthVmaImage.get(),
                                     .viewType = vk::ImageViewType::e2D,
                                     .format = m_depthVmaImage.getFormat(),
                                     .subresourceRange = depthSubresourceRange};
    auto viewResult = m_device->logical().createImageView(viewInfo);
    if (!viewResult) {
      return std::unexpected("Failed to create depth image view: " +
                             vk::to_string(viewResult.error()));
    }
    m_depthImageView = std::move(viewResult.value());
    return m_device->executeSingleTimeCommands([&](vk::CommandBuffer cmdBuffer) {
      vk::ImageMemoryBarrier barrier{.srcAccessMask = vk::AccessFlagBits::eNone,
                                     .dstAccessMask =
                                         vk::AccessFlagBits::eDepthStencilAttachmentRead |
                                         vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                                     .oldLayout = vk::ImageLayout::eUndefined,
                                     .newLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
                                     .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                                     .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                                     .image = m_depthVmaImage.get(),
                                     .subresourceRange = depthSubresourceRange};
      cmdBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                vk::PipelineStageFlagBits::eEarlyFragmentTests |
                                    vk::PipelineStageFlagBits::eLateFragmentTests,
                                {}, nullptr, nullptr, {barrier});
    });
  }

  [[nodiscard]] std::expected<void, std::string> createRenderPass() {
    if (m_useDynamicRendering) {
      m_renderPass.clear();
      return {};
    }
    vk::AttachmentDescription colorAttachment{.format = m_surfaceFormat.format,
                                              .samples = vk::SampleCountFlagBits::e1,
                                              .loadOp = vk::AttachmentLoadOp::eClear,
                                              .storeOp = vk::AttachmentStoreOp::eStore,
                                              .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
                                              .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
                                              .initialLayout = vk::ImageLayout::eUndefined,
                                              .finalLayout = vk::ImageLayout::ePresentSrcKHR};
    vk::AttachmentDescription depthAttachment{.format = m_depthFormat,
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

    m_renderPass.clear();
    auto expectedRenderPass = m_device->logical().createRenderPass(renderPassInfo);
    if (!expectedRenderPass) {
      return std::unexpected("RenderPass creation failed: " +
                             vk::to_string(expectedRenderPass.error()));
    }
    m_renderPass = std::move(expectedRenderPass.value());
    return {};
  }

  [[nodiscard]] std::expected<void, std::string> createFramebuffers() {
    if (m_useDynamicRendering) {
      for (auto &frame : m_swapchainFrames) {
        frame.framebuffer.clear();
      }
      return {};
    }
    for (auto &frame : m_swapchainFrames) {
      frame.framebuffer.clear();
      std::array<vk::ImageView, 2> attachments = {*frame.backbufferView, *m_depthImageView};
      const vk::FramebufferCreateInfo CREATE_INFO = {.renderPass = *m_renderPass,
                                                     .attachmentCount =
                                                         static_cast<u32>(attachments.size()),
                                                     .pAttachments = attachments.data(),
                                                     .width = m_swapchainExtent.width,
                                                     .height = m_swapchainExtent.height,
                                                     .layers = 1};

      auto expected = m_device->logical().createFramebuffer(CREATE_INFO);
      if (!expected) {
        return std::unexpected("Framebuffer creation failed: " + vk::to_string(expected.error()));
      }
      frame.framebuffer = std::move(expected.value());
    }
    return {};
  }

  // Renamed from createSyncObjects to createPerFrameSyncObjects
  [[nodiscard]] std::expected<void, std::string> createPerFrameSyncObjects() {
    for (auto &frame : m_perFrame) {
      if (auto pool = m_device->logical().createCommandPool(
              {.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
               .queueFamilyIndex = m_device->graphicsQueue().queueFamily()});
          pool) {
        frame.commandPool = std::move(*pool);
      } else {
        return std::unexpected("Failed to create command pool: " + vk::to_string(pool.error()));
      }
      if (auto buffers =
              m_device->logical().allocateCommandBuffers({.commandPool = *frame.commandPool,
                                                          .level = vk::CommandBufferLevel::ePrimary,
                                                          .commandBufferCount = 1});
          buffers) {
        frame.commandBuffer = std::move(buffers->front());
      } else {
        return std::unexpected("Failed to allocate command buffer: " +
                               vk::to_string(buffers.error()));
      }
      if (auto fence =
              m_device->logical().createFence({.flags = vk::FenceCreateFlagBits::eSignaled});
          fence) {
        frame.fence = std::move(*fence);
      } else {
        return std::unexpected("Failed to create fence: " + vk::to_string(fence.error()));
      }
      if (auto imageSem = m_device->logical().createSemaphore({}); imageSem) {
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
  VulkanDevice *m_device{};
  vk::raii::SurfaceKHR m_surface{nullptr};
  bool m_firstCreation{true};
  bool m_resizeNeeded{false};

  vk::raii::SwapchainKHR m_swapchain{nullptr};
  std::vector<SwapchainFrame> m_swapchainFrames;
  vk::Extent2D m_swapchainExtent{};
  vk::SurfaceFormatKHR m_surfaceFormat{};
  vk::PresentModeKHR m_presentMode{};
  bool m_useDynamicRendering{};

  VmaImage m_depthVmaImage;
  vk::raii::ImageView m_depthImageView{nullptr};
  vk::Format m_depthFormat{};

  vk::raii::RenderPass m_renderPass{nullptr};

  vk::ClearValue m_clearValue{vk::ClearColorValue(std::array<f32, 4>{0.0, 0.0, 0.0, 1.0})};

  static constexpr u32 MAX_FRAMES_IN_FLIGHT = 2;
  std::array<PerFrame, MAX_FRAMES_IN_FLIGHT> m_perFrame;
  u32 m_currentFrame{0};
  u32 m_imageIndex{0};
};
