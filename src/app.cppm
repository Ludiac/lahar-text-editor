module;

#define GLM_ENABLE_EXPERIMENTAL
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "macros.hpp"
#include "primitive_types.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

export module vulkan_app;
export import :SDLWrapper;

import vulkan_hpp;
import std;
import BS.thread_pool;

import :VulkanWindow;
import :VulkanDevice;
import :VulkanInstance;
import :VulkanPipeline;
import :utils;
import :imgui;
import :TextSystem;
import :UISystem;
import :ui;
import :TextArea;
import :TextView;
import :ThreeDEngine; // Import the new 3D engine module

namespace {
constexpr u32 MIN_IMAGE_COUNT = 2;

std::atomic<bool> gImguiFatalErrorFlag{false};

void checkVkResultForImgui(VkResult err) {
  if (err == VK_SUCCESS) {
    return;
  }

  std::string errMsg = "[vulkan] Error: VkResult = " + std::to_string(err);
  std::println("{}", errMsg);
  gImguiFatalErrorFlag.store(true);
}

} // anonymous namespace

export class App {
  VulkanInstance m_instance;
  VulkanDevice m_device{m_instance};
  VulkanWindow m_wd;
  bool m_swapChainRebuild = false;

  // 3D Engine
  std::unique_ptr<ThreeDEngine> m_threeDEngine;

  // 2D Rendering Systems
  VulkanPipeline m_textPipeline;
  VulkanPipeline m_uiPipeline;
  vk::raii::PipelineCache m_pipelineCache{nullptr};
  TextSystem m_textSystem;
  UISystem m_uiSystem;

  // Text Editor MVC Components
  TextEditor m_textEditor{
      "Lorem ipsum vulkan\n Lorem ipsum vulkan Lorem\n ipsum vulkan Lorem ipsum "
      "vulkan Lorem ipsum\n vulkan"};
  std::unique_ptr<TextView> m_textView;
  std::vector<Font *> m_registeredFonts;

  // Async Asset Loading
  BS::thread_pool<> m_thread_pool;
  std::vector<std::future<std::expected<Font *, std::string>>> m_fontLoadFutures;
  std::mutex m_fontFuturesMutex;

  // UI State
  i32 m_fontSizeMultiplier{0};
  TextToggles m_textToggles;

  ImGuiMenu m_imguiMenu;

private:
  // All initialization functions that can fail are changed to return a std::expected.
  // This allows us to propagate errors up to the main `run` function without calling std::exit.
  [[nodiscard]] std::expected<void, std::string> create2DGraphicsPipelines() {
    auto textVertShaderResult =
        createShaderModuleFromFile(m_device.logical(), "shaders/text_vert.spv");
    if (!textVertShaderResult) {
      return std::unexpected(textVertShaderResult.error());
    }
    auto textFragShaderResult =
        createShaderModuleFromFile(m_device.logical(), "shaders/text_frag.spv");
    if (!textFragShaderResult) {
      return std::unexpected(textFragShaderResult.error());
    }
    auto textVertShader = std::move(*textVertShaderResult);
    auto textFragShader = std::move(*textFragShaderResult);

    vk::raii::DescriptorSetLayout textSetLayout{nullptr};
    std::vector<vk::DescriptorSetLayoutBinding> textBindings = {
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eFragment}};
    vk::DescriptorSetLayoutCreateInfo textLayoutInfo{
        .bindingCount = static_cast<u32>(textBindings.size()), .pBindings = textBindings.data()};

    auto textSetLayoutResult = m_device.logical().createDescriptorSetLayout(textLayoutInfo);
    if (!textSetLayoutResult) {
      return std::unexpected("Failed to create text descriptor set layout.");
    }

    textSetLayout = std::move(textSetLayoutResult.value());

    auto uiVertShaderResult = createShaderModuleFromFile(m_device.logical(), "shaders/ui_vert.spv");
    if (!uiVertShaderResult) {
      return std::unexpected(uiVertShaderResult.error());
    }
    auto uiFragShaderResult = createShaderModuleFromFile(m_device.logical(), "shaders/ui_frag.spv");
    if (!uiFragShaderResult) {
      return std::unexpected(uiFragShaderResult.error());
    }
    auto uiVertShader = std::move(*uiVertShaderResult);
    auto uiFragShader = std::move(*uiFragShaderResult);

    vk::raii::DescriptorSetLayout instanceSetLayout{nullptr};
    std::vector<vk::DescriptorSetLayoutBinding> instanceBindings = {
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eStorageBufferDynamic,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eVertex}};
    vk::DescriptorSetLayoutCreateInfo instanceLayoutInfo{
        .bindingCount = static_cast<u32>(instanceBindings.size()),
        .pBindings = instanceBindings.data()};

    auto instanceSetLayoutResult = m_device.logical().createDescriptorSetLayout(instanceLayoutInfo);
    if (!instanceSetLayoutResult) {
      return std::unexpected("Failed to create instance descriptor set layout.");
    }
    instanceSetLayout = std::move(instanceSetLayoutResult.value());

    vk::PushConstantRange textPushConstantRange{.stageFlags = vk::ShaderStageFlagBits::eVertex |
                                                              vk::ShaderStageFlagBits::eFragment,
                                                .offset = 0,
                                                .size = sizeof(std::array<std::byte, 128>)};
    std::vector<vk::DescriptorSetLayout> textLayouts = {*instanceSetLayout, *textSetLayout};
    auto textPipelineLayoutResult = m_textPipeline.createPipelineLayout(
        m_device.logical(), textLayouts, {textPushConstantRange});
    if (!textPipelineLayoutResult) {
      return std::unexpected(textPipelineLayoutResult.error());
    }

    vk::PushConstantRange uiPushConstantRange{.stageFlags = vk::ShaderStageFlagBits::eVertex,
                                              .offset = 0,
                                              .size = sizeof(std::array<std::byte, 128>)};
    std::vector<vk::DescriptorSetLayout> uiLayouts = {*instanceSetLayout};
    auto uiPipelineLayoutResult =
        m_uiPipeline.createPipelineLayout(m_device.logical(), uiLayouts, {uiPushConstantRange});
    if (!uiPipelineLayoutResult) {
      return std::unexpected(uiPipelineLayoutResult.error());
    }

    std::vector<vk::PipelineShaderStageCreateInfo> textShaderStages = {
        {.stage = vk::ShaderStageFlagBits::eVertex, .module = *textVertShader, .pName = "main"},
        {.stage = vk::ShaderStageFlagBits::eFragment, .module = *textFragShader, .pName = "main"}};
    std::vector<vk::PipelineShaderStageCreateInfo> uiShaderStages = {
        {.stage = vk::ShaderStageFlagBits::eVertex, .module = *uiVertShader, .pName = "main"},
        {.stage = vk::ShaderStageFlagBits::eFragment, .module = *uiFragShader, .pName = "main"}};

    vk::VertexInputBindingDescription textBindingDesc{
        .binding = 0, .stride = sizeof(TextQuadVertex), .inputRate = vk::VertexInputRate::eVertex};
    std::array<vk::VertexInputAttributeDescription, 2> textAttrDesc;
    textAttrDesc[0] = {
        .location = 0, .binding = 0, .format = vk::Format::eR32G32Sfloat, .offset = 0};
    textAttrDesc[1] = {.location = 1,
                       .binding = 0,
                       .format = vk::Format::eR32G32Sfloat,
                       .offset = offsetof(TextQuadVertex, uv)};

    vk::PipelineVertexInputStateCreateInfo vertexInputInfo{
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &textBindingDesc,
        .vertexAttributeDescriptionCount = 2,
        .pVertexAttributeDescriptions = textAttrDesc.data()};
    vk::PipelineInputAssemblyStateCreateInfo inputAssembly{
        .topology = vk::PrimitiveTopology::eTriangleList};
    vk::PipelineColorBlendAttachmentState blendAttachment{
        .blendEnable = vk::True,
        .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
        .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eOne,
        .dstAlphaBlendFactor = vk::BlendFactor::eZero,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
    vk::PipelineDepthStencilStateCreateInfo depthStencil{.depthTestEnable = vk::False,
                                                         .depthWriteEnable = vk::False};

    auto textGraphicsPipelineResult = m_textPipeline.createGraphicsPipeline(
        m_device.logical(), m_pipelineCache, textShaderStages, vertexInputInfo, inputAssembly,
        m_wd.getRenderPass(), &blendAttachment, &depthStencil);
    if (!textGraphicsPipelineResult) {
      return std::unexpected(textGraphicsPipelineResult.error());
    }
    auto uiGraphicsPipelineResult = m_uiPipeline.createGraphicsPipeline(
        m_device.logical(), m_pipelineCache, uiShaderStages, vertexInputInfo, inputAssembly,
        m_wd.getRenderPass(), &blendAttachment, &depthStencil);
    if (!uiGraphicsPipelineResult) {
      return std::unexpected(uiGraphicsPipelineResult.error());
    }

    return {}; // Success
  }

  [[nodiscard]] std::expected<void, std::string> setupVulkan() {
    // Each of these methods in the classes `VulkanInstance` and `VulkanDevice`
    // should also be refactored to return std::expected instead of calling std::exit.
    if (auto res = m_instance.create(); !res) {
      return std::unexpected(res.error());
    }
    if (auto res = m_instance.setupDebugMessenger(); !res) {
      return std::unexpected(res.error());
    }
    if (auto res = m_device.pickPhysicalDevice(); !res) {
      return std::unexpected(res.error());
    }
    if (auto res = m_device.createLogicalDevice(); !res) {
      return std::unexpected(res.error());
    }

    auto cacheResult = m_device.logical().createPipelineCache({});
    if (!cacheResult) {
      return std::unexpected("Failed to create pipeline cache: " +
                             vk::to_string(cacheResult.error()));
    }
    m_pipelineCache = std::move(cacheResult.value());

    return {}; // Success
  }

  [[nodiscard]] std::expected<void, std::string> setupVulkanWindow(SDL_Window *sdlWindow,
                                                                   vk::Extent2D extent) {
    VkSurfaceKHR surfaceRawHandle = nullptr;
    if (!SDL_Vulkan_CreateSurface(sdlWindow, m_instance.getCHandle(), nullptr, &surfaceRawHandle)) {
      return std::unexpected("Failed to create Vulkan surface via SDL: " +
                             std::string(SDL_GetError()));
    }

    m_wd = std::move(
        VulkanWindow{m_device, vk::raii::SurfaceKHR(m_instance, surfaceRawHandle), false});
    if (auto exp = m_wd.createOrResize(extent, MIN_IMAGE_COUNT); !exp) {
      return exp;
    }
    return {}; // Success
  }

  void frameRender(ImDrawData *drawData, f32 deltaTime) {
    auto frameStatus = m_wd.renderFrame([&](vk::raii::CommandBuffer &cmd, vk::raii::RenderPass &rp,
                                            vk::raii::Framebuffer &fb) {
      // Update and draw 3D scene
      if (m_threeDEngine) {
        m_threeDEngine->update(m_wd.getImageIndex(), deltaTime, m_wd.getExtent());
      }

      std::array<vk::ClearValue, 2> clearValues{};
      clearValues[0].color = m_wd.clearValue.color;
      clearValues[1].depthStencil = {.depth = 1.0, .stencil = 0};

      cmd.beginRenderPass({.renderPass = rp,
                           .framebuffer = fb,
                           .renderArea = {.offset = {.x = 0, .y = 0}, .extent = m_wd.getExtent()},
                           .clearValueCount = static_cast<u32>(clearValues.size()),
                           .pClearValues = clearValues.data()},
                          vk::SubpassContents::eInline);
      cmd.setViewport(0, vk::Viewport{.x = 0.0,
                                      .y = 0.0,
                                      .width = static_cast<f32>(m_wd.getExtent().width),
                                      .height = static_cast<f32>(m_wd.getExtent().height),
                                      .minDepth = 0.0,
                                      .maxDepth = 1.0});
      cmd.setScissor(0, vk::Rect2D{.offset = {.x = 0, .y = 0}, .extent = m_wd.getExtent()});

      if (m_threeDEngine) {
        m_threeDEngine->draw(cmd, m_wd.getImageIndex());
      }

      // Prepare and draw 2D elements
      m_textSystem.beginFrame();
      m_uiSystem.beginFrame();
      RenderQueue renderQueue;

      if (m_textView) {
        m_textView->width = 5000.0;
        m_textView->height = 3000.0;
        usize firstLine = m_textView->getFirstVisibleLine();
        usize numLines = m_textView->getVisibleLineCount();
        usize lastLine = std::min(firstLine + numLines, m_textEditor.lineCount());
        f64 currentLineYpos = 100.0;

        if (!m_registeredFonts.empty()) {
          for (usize i = firstLine; i < lastLine; ++i) {
            m_textSystem.queueText(m_registeredFonts[0], m_textEditor.getLine(i),
                                   static_cast<u32>(m_textView->fontPointSize), 200.0,
                                   currentLineYpos, {1.0, 1.0, 1.0, 1.0});
            const f64 POINT_SIZE = 36.0;
            const f64 FONT_UNIT_TO_PIXEL_SCALE =
                POINT_SIZE * (96.0 / 72.0 * 2) / m_registeredFonts[0]->atlasData.unitsPerEm;
            const f64 LINE_HEIGHT_PX =
                m_registeredFonts[0]->atlasData.lineHeight * FONT_UNIT_TO_PIXEL_SCALE;
            currentLineYpos += (LINE_HEIGHT_PX > 0) ? LINE_HEIGHT_PX : 38.0;
          }
        }
      }

      m_uiSystem.queueQuad({.position = {200, 200},
                            .size = {300, 300},
                            .color = {0.0, 1.0, 0.0, 1.0},
                            .zLayer = 0.0});
      m_uiSystem.queueQuad({.position = {1000, 200},
                            .size = {100, 300},
                            .color = {0.0, 0.0, 1.0, 1.0},
                            .zLayer = 0.0});

      m_textSystem.prepareBatches(renderQueue, m_textPipeline, m_wd.getImageIndex(),
                                  m_wd.getExtent(), m_textToggles);
      m_uiSystem.prepareBatches(renderQueue, m_uiPipeline, m_wd.getImageIndex(), m_wd.getExtent());
      processRenderQueue(cmd, renderQueue);

      ImGui_ImplVulkan_RenderDrawData(drawData, *cmd);
      cmd.endRenderPass();
    });

    if (frameStatus == VulkanWindow::FrameStatus::ResizeNeeded) {
      m_swapChainRebuild = true;
    } else if (frameStatus == VulkanWindow::FrameStatus::Error) {
      std::println("Error during frame rendering.");
    }
  }

  static vk::Extent2D getWindowSize(SDL_Window *window) {
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    return {.width = static_cast<u32>(width > 0 ? width : 1),
            .height = static_cast<u32>(height > 0 ? height : 1)};
  }

  void readKeyboard(f32 deltaTime, SDL_Window *sdlWindow) {
    static bool isFullscreen = false;
    const auto *const KEYSTATE = SDL_GetKeyboardState(nullptr);
    if (m_threeDEngine) {
      auto &camera = m_threeDEngine->getCamera();
      f32 velocity = camera.movementSpeed * deltaTime;
      if (KEYSTATE[SDL_SCANCODE_W]) {
        camera.position += camera.front * velocity;
      }
      if (KEYSTATE[SDL_SCANCODE_S]) {
        camera.position -= camera.front * velocity;
      }
      if (KEYSTATE[SDL_SCANCODE_A]) {
        camera.position -= camera.right * velocity;
      }
      if (KEYSTATE[SDL_SCANCODE_D]) {
        camera.position += camera.right * velocity;
      }
      if (KEYSTATE[SDL_SCANCODE_SPACE]) {
        camera.position += camera.worldUp * velocity;
      }
      if (KEYSTATE[SDL_SCANCODE_LCTRL]) {
        camera.position -= camera.worldUp * velocity;
      }
    }
    if (KEYSTATE[SDL_SCANCODE_F11]) {
      isFullscreen = !isFullscreen;
      SDL_SetWindowFullscreen(sdlWindow, isFullscreen);
    }
  }

  void mainLoop(SDL_Window *sdlWindow) {
    ImGuiIO &imguiIO = ImGui::GetIO();
    imguiIO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // imguiIO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    using Clock = std::chrono::high_resolution_clock;
    auto previousTime = Clock::now();
    f32 deltaTime = 0.0;
    bool done = false;

    while (!done) {
      // Check the atomic flag. If ImGui's Vulkan init failed, we exit the loop.
      if (gImguiFatalErrorFlag.load()) {
        done = true;
      }

      auto currentTime = Clock::now();
      deltaTime = std::chrono::duration<f32>(currentTime - previousTime).count();
      previousTime = currentTime;

      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                                             event.window.windowID == SDL_GetWindowID(sdlWindow))) {
          done = true;
        }
        if (event.type == SDL_EVENT_WINDOW_MINIMIZED) {
        }
        if (event.type == SDL_EVENT_WINDOW_RESTORED || event.type == SDL_EVENT_WINDOW_RESIZED ||
            event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
          m_swapChainRebuild = true;
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL && m_textView) {
          m_textView->scroll(event.wheel.y > 0 ? -3 : 3);
        }
      }

      if (m_threeDEngine) {
        m_threeDEngine->processGltfLoads();
      }

      {
        std::lock_guard lock(m_fontFuturesMutex);
        for (auto it = m_fontLoadFutures.begin(); it != m_fontLoadFutures.end();) {
          if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto res = it->get();
            if (res) {
              Font *font = *res;
              m_registeredFonts.emplace_back(font);
              if (!m_textView && (font != nullptr)) {
                m_textView = std::make_unique<TextView>(m_textEditor, *font);
              }
            } else {
              std::println("failed to load font asynchronously: {}", res.error());
            }
            it = m_fontLoadFutures.erase(it);
          }
        }
      }

      if (m_threeDEngine) {
        readKeyboard(deltaTime, sdlWindow);
      }

      if ((SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_MINIMIZED) != 0U) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      vk::Extent2D currentExtent = getWindowSize(sdlWindow);
      if (currentExtent.width == 0 || currentExtent.height == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      if (m_swapChainRebuild) {
        m_device.logical().waitIdle();
        if (auto exp = m_wd.createOrResize(currentExtent, MIN_IMAGE_COUNT); !exp) {
          std::println("Failed to recreate swapchain: {}", exp.error());
          // Handle this error more gracefully, perhaps by attempting to reinitialize or exit.
          done = true; // For now, we'll exit.
        }
        if (m_threeDEngine) {
          m_threeDEngine->onSwapchainRecreated(m_wd.getImageCount());
        }
        m_swapChainRebuild = false;
      }

      ImGui_ImplVulkan_NewFrame();
      ImGui_ImplSDL3_NewFrame();
      ImGui::NewFrame();

      m_imguiMenu.renderMegaMenu(
          m_imguiMenu, m_wd, m_device, m_threeDEngine->getCamera(), m_threeDEngine->getScene(),
          m_textSystem, m_threeDEngine->getShaderToggles(), m_threeDEngine->getLightUbo(),
          m_textToggles, m_fontSizeMultiplier, m_wd.getCurrentFrame(), deltaTime);

      ImGui::Render();
      frameRender(ImGui::GetDrawData(), deltaTime);
    }
  }

public:
  // The main entry point for the application. It returns an integer status code.
  // 0 for success, non-zero for failure.
  int run(SDL_Window *sdlWindow) {
    // We now check the result of each initialization step.
    // If a step fails, we print the error and return a non-zero exit code.
    if (auto res = setupVulkan(); !res) {
      std::println("Vulkan setup failed: {}", res.error());
      return 1;
    }
    if (auto res = setupVulkanWindow(sdlWindow, getWindowSize(sdlWindow)); !res) {
      std::println("Vulkan window setup failed: {}", res.error());
      return 1;
    }

    u32 numMeshesEstimate = 100;
    // Assume device.createDescriptorPool also returns std::expected
    auto createDescriptorPoolResult =
        m_device.createDescriptorPool(static_cast<u32>(m_wd.getImageCount()) * numMeshesEstimate);
    if (!createDescriptorPoolResult) {
      std::println("Failed to create descriptor pool: {}", createDescriptorPoolResult.error());
      return 1;
    }

    // Initialize Engines
    m_threeDEngine = std::make_unique<ThreeDEngine>(m_device, m_wd.getImageCount(), m_thread_pool);
    // Assume threeDEngine->initialize also returns std::expected
    if (auto res = m_threeDEngine->initialize(m_wd.getRenderPass(), m_pipelineCache); !res) {
      std::println("3D engine initialization failed: {}", res.error());
      return 1;
    }
    m_threeDEngine->loadInitialAssets();

    if (auto res = create2DGraphicsPipelines(); !res) {
      std::println("2D graphics pipeline creation failed: {}", res.error());
      return 1;
    }

    auto textSystemExpected =
        TextSystem::create(m_device, m_wd.getImageCount(), m_device.descriptorPool);
    if (!textSystemExpected) {
      std::println("textSystem creation failed: {}", textSystemExpected.error());
      return 1;
    }
    m_textSystem = std::move(textSystemExpected.value());

    auto uiSystemExpected =
        UISystem::create(m_device, m_wd.getImageCount(), m_device.descriptorPool);
    if (!uiSystemExpected) {
      std::println("uiSystem creation failed: {}", uiSystemExpected.error());
      return 1;
    }
    m_uiSystem = std::move(uiSystemExpected.value());

    // Load 2D assets
    {
      std::lock_guard lock(m_fontFuturesMutex);
      m_fontLoadFutures.emplace_back(m_thread_pool.submit_task([this]() {
        // This is a bit of a hack, we need to get the layout from the pipeline
        // but the pipeline is created after the text system.
        // For now, we assume the layout is known.
        vk::raii::DescriptorSetLayout textSetLayout{nullptr};
        std::vector<vk::DescriptorSetLayoutBinding> textBindings = {
            {.binding = 0,
             .descriptorType = vk::DescriptorType::eCombinedImageSampler,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eFragment}};
        vk::DescriptorSetLayoutCreateInfo textLayoutInfo{.bindingCount =
                                                             static_cast<u32>(textBindings.size()),
                                                         .pBindings = textBindings.data()};
        textSetLayout = m_device.logical().createDescriptorSetLayout(textLayoutInfo).value();
        return m_textSystem.registerFont(
            "../../assets/fonts/Inconsolata/InconsolataNerdFontMono-Regular.ttf", 64,
            textSetLayout);
      }));
    }

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForVulkan(sdlWindow);
    ImGui_ImplVulkan_InitInfo initInfo = m_device.initInfo();
    initInfo.Instance = m_instance.getCHandle();
    initInfo.RenderPass = *m_wd.getRenderPass();
    initInfo.MinImageCount = MIN_IMAGE_COUNT;
    initInfo.ImageCount = static_cast<u32>(m_wd.getImageCount());
    initInfo.PipelineCache = *m_pipelineCache;
    initInfo.CheckVkResultFn = checkVkResultForImgui;
    ImGui_ImplVulkan_Init(&initInfo);
    // ImGui_ImplVulkan_CreateFontsTexture();

    mainLoop(sdlWindow);

    m_device.logical().waitIdle();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    return 0; // Success
  }
};
