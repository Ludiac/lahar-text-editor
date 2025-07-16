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

std::atomic<bool> g_imgui_fatal_error_flag{false};

void check_vk_result_for_imgui(VkResult err) {
  if (err == VK_SUCCESS) {
    return;
  }

  std::string errMsg = "[vulkan] Error: VkResult = " + std::to_string(err);
  std::println("{}", errMsg);
  g_imgui_fatal_error_flag.store(true);
}

} // anonymous namespace

export class App {
  VulkanInstance instance;
  VulkanDevice device{instance};
  VulkanWindow wd;
  bool swapChainRebuild = false;

  // 3D Engine
  std::unique_ptr<ThreeDEngine> threeDEngine;

  // 2D Rendering Systems
  VulkanPipeline textPipeline;
  VulkanPipeline uiPipeline;
  vk::raii::PipelineCache pipelineCache{nullptr};
  TextSystem textSystem;
  UISystem uiSystem;

  // Text Editor MVC Components
  TextEditor textEditor{"Lorem ipsum vulkan\n Lorem ipsum vulkan Lorem\n ipsum vulkan Lorem ipsum "
                        "vulkan Lorem ipsum\n vulkan"};
  std::unique_ptr<TextView> textView;
  std::vector<Font *> registeredFonts;

  // Async Asset Loading
  BS::thread_pool<> thread_pool;
  std::vector<std::future<std::expected<Font *, std::string>>> fontLoadFutures;
  std::mutex fontFuturesMutex;

  // UI State
  i32 fontSizeMultiplier{0};
  TextToggles textToggles;

  ImGuiMenu imguiMenu;

private:
  // All initialization functions that can fail are changed to return a std::expected.
  // This allows us to propagate errors up to the main `run` function without calling std::exit.
  [[nodiscard]] std::expected<void, std::string> create2DGraphicsPipelines() {
    auto textVertShaderResult =
        createShaderModuleFromFile(device.logical(), "shaders/text_vert.spv");
    if (!textVertShaderResult)
      return std::unexpected(textVertShaderResult.error());
    auto textFragShaderResult =
        createShaderModuleFromFile(device.logical(), "shaders/text_frag.spv");
    if (!textFragShaderResult)
      return std::unexpected(textFragShaderResult.error());
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

    auto textSetLayoutResult = device.logical().createDescriptorSetLayout(textLayoutInfo);
    if (!textSetLayoutResult)
      return std::unexpected("Failed to create text descriptor set layout.");

    textSetLayout = std::move(textSetLayoutResult.value());

    auto uiVertShaderResult = createShaderModuleFromFile(device.logical(), "shaders/ui_vert.spv");
    if (!uiVertShaderResult)
      return std::unexpected(uiVertShaderResult.error());
    auto uiFragShaderResult = createShaderModuleFromFile(device.logical(), "shaders/ui_frag.spv");
    if (!uiFragShaderResult)
      return std::unexpected(uiFragShaderResult.error());
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

    auto instanceSetLayoutResult = device.logical().createDescriptorSetLayout(instanceLayoutInfo);
    if (!instanceSetLayoutResult) {
      return std::unexpected("Failed to create instance descriptor set layout.");
    }
    instanceSetLayout = std::move(instanceSetLayoutResult.value());

    vk::PushConstantRange textPushConstantRange{.stageFlags = vk::ShaderStageFlagBits::eVertex |
                                                              vk::ShaderStageFlagBits::eFragment,
                                                .offset = 0,
                                                .size = sizeof(std::array<std::byte, 128>)};
    std::vector<vk::DescriptorSetLayout> textLayouts = {*instanceSetLayout, *textSetLayout};
    auto textPipelineLayoutResult =
        textPipeline.createPipelineLayout(device.logical(), textLayouts, {textPushConstantRange});
    if (!textPipelineLayoutResult) {
      return std::unexpected(textPipelineLayoutResult.error());
    }

    vk::PushConstantRange uiPushConstantRange{.stageFlags = vk::ShaderStageFlagBits::eVertex,
                                              .offset = 0,
                                              .size = sizeof(std::array<std::byte, 128>)};
    std::vector<vk::DescriptorSetLayout> uiLayouts = {*instanceSetLayout};
    auto uiPipelineLayoutResult =
        uiPipeline.createPipelineLayout(device.logical(), uiLayouts, {uiPushConstantRange});
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

    auto textGraphicsPipelineResult = textPipeline.createGraphicsPipeline(
        device.logical(), pipelineCache, textShaderStages, vertexInputInfo, inputAssembly,
        wd.getRenderPass(), &blendAttachment, &depthStencil);
    if (!textGraphicsPipelineResult) {
      return std::unexpected(textGraphicsPipelineResult.error());
    }
    auto uiGraphicsPipelineResult = uiPipeline.createGraphicsPipeline(
        device.logical(), pipelineCache, uiShaderStages, vertexInputInfo, inputAssembly,
        wd.getRenderPass(), &blendAttachment, &depthStencil);
    if (!uiGraphicsPipelineResult) {
      return std::unexpected(uiGraphicsPipelineResult.error());
    }

    return {}; // Success
  }

  [[nodiscard]] std::expected<void, std::string> SetupVulkan() {
    // Each of these methods in the classes `VulkanInstance` and `VulkanDevice`
    // should also be refactored to return std::expected instead of calling std::exit.
    if (auto res = instance.create(); !res)
      return std::unexpected(res.error());
    if (auto res = instance.setupDebugMessenger(); !res)
      return std::unexpected(res.error());
    if (auto res = device.pickPhysicalDevice(); !res)
      return std::unexpected(res.error());
    if (auto res = device.createLogicalDevice(); !res)
      return std::unexpected(res.error());

    auto cacheResult = device.logical().createPipelineCache({});
    if (!cacheResult) {
      return std::unexpected("Failed to create pipeline cache: " +
                             vk::to_string(cacheResult.error()));
    }
    pipelineCache = std::move(cacheResult.value());

    return {}; // Success
  }

  [[nodiscard]] std::expected<void, std::string> SetupVulkanWindow(SDL_Window *sdl_window,
                                                                   vk::Extent2D extent) {
    VkSurfaceKHR surface_raw_handle = nullptr;
    if (!SDL_Vulkan_CreateSurface(sdl_window, instance.get_C_handle(), nullptr,
                                  &surface_raw_handle)) {
      return std::unexpected("Failed to create Vulkan surface via SDL: " +
                             std::string(SDL_GetError()));
    }

    wd = std::move(VulkanWindow{device, vk::raii::SurfaceKHR(instance, surface_raw_handle), false});
    if (auto exp = wd.createOrResize(extent, MIN_IMAGE_COUNT); !exp) {
      return exp;
    }
    return {}; // Success
  }

  void FrameRender(ImDrawData *draw_data, f32 deltaTime) {
    auto frameStatus = wd.renderFrame([&](vk::raii::CommandBuffer &cmd, vk::raii::RenderPass &rp,
                                          vk::raii::Framebuffer &fb) {
      // Update and draw 3D scene
      if (threeDEngine) {
        threeDEngine->update(wd.getImageIndex(), deltaTime, wd.getExtent());
      }

      std::array<vk::ClearValue, 2> clearValues{};
      clearValues[0].color = wd.clearValue.color;
      clearValues[1].depthStencil = {.depth = 1.0, .stencil = 0};

      cmd.beginRenderPass({.renderPass = rp,
                           .framebuffer = fb,
                           .renderArea = {.offset = {.x = 0, .y = 0}, .extent = wd.getExtent()},
                           .clearValueCount = static_cast<u32>(clearValues.size()),
                           .pClearValues = clearValues.data()},
                          vk::SubpassContents::eInline);
      cmd.setViewport(0, vk::Viewport{.x = 0.0,
                                      .y = 0.0,
                                      .width = static_cast<f32>(wd.getExtent().width),
                                      .height = static_cast<f32>(wd.getExtent().height),
                                      .minDepth = 0.0,
                                      .maxDepth = 1.0});
      cmd.setScissor(0, vk::Rect2D{.offset = {.x = 0, .y = 0}, .extent = wd.getExtent()});

      if (threeDEngine) {
        threeDEngine->draw(cmd, wd.getImageIndex());
      }

      // Prepare and draw 2D elements
      textSystem.beginFrame();
      uiSystem.beginFrame();
      RenderQueue renderQueue;

      if (textView) {
        textView->setDimensions(5000.0, 3000.0);
        usize firstLine = textView->getFirstVisibleLine();
        usize numLines = textView->getVisibleLineCount();
        usize lastLine = std::min(firstLine + numLines, textEditor.lineCount());
        f32 currentLineYpos = 100.0;

        if (!registeredFonts.empty()) {
          for (usize i = firstLine; i < lastLine; ++i) {
            textSystem.queueText(registeredFonts[0], textEditor.getLine(i), textView->fontPointSize,
                                 200.0, currentLineYpos, {1.0, 1.0, 1.0, 1.0});
            const f64 pointSize = 36.0;
            const f64 fontUnitToPixelScale =
                pointSize * (96.0 / 72.0 * 2) / registeredFonts[0]->atlasData.unitsPerEm;
            const f64 line_height_px =
                registeredFonts[0]->atlasData.lineHeight * fontUnitToPixelScale;
            currentLineYpos += (line_height_px > 0) ? static_cast<f32>(line_height_px) : 38.0;
          }
        }
      }

      uiSystem.queueQuad({.position = {200, 200},
                          .size = {300, 300},
                          .color = {0.0, 1.0, 0.0, 1.0},
                          .z_layer = 0.0});
      uiSystem.queueQuad({.position = {1000, 200},
                          .size = {100, 300},
                          .color = {0.0, 0.0, 1.0, 1.0},
                          .z_layer = 0.0});

      textSystem.prepareBatches(renderQueue, textPipeline, wd.getImageIndex(), wd.getExtent(),
                                textToggles);
      uiSystem.prepareBatches(renderQueue, uiPipeline, wd.getImageIndex(), wd.getExtent());
      processRenderQueue(cmd, renderQueue);

      ImGui_ImplVulkan_RenderDrawData(draw_data, *cmd);
      cmd.endRenderPass();
    });

    if (frameStatus == VulkanWindow::FrameStatus::ResizeNeeded) {
      swapChainRebuild = true;
    } else if (frameStatus == VulkanWindow::FrameStatus::Error) {
      std::println("Error during frame rendering.");
    }
  }

  static vk::Extent2D get_window_size(SDL_Window *window) {
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    return {.width = static_cast<u32>(width > 0 ? width : 1),
            .height = static_cast<u32>(height > 0 ? height : 1)};
  }

  void readKeyboard(f32 deltaTime, SDL_Window *sdl_window) {
    static bool isFullscreen = false;
    const auto *const keystate = SDL_GetKeyboardState(nullptr);
    if (threeDEngine) {
      auto &camera = threeDEngine->getCamera();
      f32 velocity = camera.MovementSpeed * deltaTime;
      if (keystate[SDL_SCANCODE_W]) {
        camera.Position += camera.Front * velocity;
      }
      if (keystate[SDL_SCANCODE_S]) {
        camera.Position -= camera.Front * velocity;
      }
      if (keystate[SDL_SCANCODE_A]) {
        camera.Position -= camera.Right * velocity;
      }
      if (keystate[SDL_SCANCODE_D]) {
        camera.Position += camera.Right * velocity;
      }
      if (keystate[SDL_SCANCODE_SPACE]) {
        camera.Position += camera.WorldUp * velocity;
      }
      if (keystate[SDL_SCANCODE_LCTRL]) {
        camera.Position -= camera.WorldUp * velocity;
      }
    }
    if (keystate[SDL_SCANCODE_F11]) {
      isFullscreen = !isFullscreen;
      SDL_SetWindowFullscreen(sdl_window, isFullscreen);
    }
  }

  void mainLoop(SDL_Window *sdl_window) {
    ImGuiIO &imguiIO = ImGui::GetIO();
    imguiIO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // imguiIO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    using Clock = std::chrono::high_resolution_clock;
    auto previousTime = Clock::now();
    f32 deltaTime = 0.0;
    bool done = false;

    while (!done) {
      // Check the atomic flag. If ImGui's Vulkan init failed, we exit the loop.
      if (g_imgui_fatal_error_flag.load()) {
        done = true;
      }

      auto currentTime = Clock::now();
      deltaTime = std::chrono::duration<f32>(currentTime - previousTime).count();
      previousTime = currentTime;

      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT ||
            (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
             event.window.windowID == SDL_GetWindowID(sdl_window))) {
          done = true;
        }
        if (event.type == SDL_EVENT_WINDOW_MINIMIZED) {
        }
        if (event.type == SDL_EVENT_WINDOW_RESTORED || event.type == SDL_EVENT_WINDOW_RESIZED ||
            event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
          swapChainRebuild = true;
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL && textView) {
          textView->scroll(event.wheel.y > 0 ? -3 : 3);
        }
      }

      if (threeDEngine) {
        threeDEngine->processGltfLoads();
      }

      {
        std::lock_guard lock(fontFuturesMutex);
        for (auto it = fontLoadFutures.begin(); it != fontLoadFutures.end();) {
          if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto res = it->get();
            if (res) {
              Font *font = *res;
              registeredFonts.emplace_back(font);
              if (!textView && (font != nullptr)) {
                textView = std::make_unique<TextView>(textEditor, *font);
              }
            } else {
              std::println("failed to load font asynchronously: {}", res.error());
            }
            it = fontLoadFutures.erase(it);
          }
        }
      }

      if (threeDEngine) {
        readKeyboard(deltaTime, sdl_window);
      }

      if ((SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_MINIMIZED) != 0U) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      vk::Extent2D currentExtent = get_window_size(sdl_window);
      if (currentExtent.width == 0 || currentExtent.height == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      if (swapChainRebuild) {
        device.logical().waitIdle();
        if (auto exp = wd.createOrResize(currentExtent, MIN_IMAGE_COUNT); !exp) {
          std::println("Failed to recreate swapchain: {}", exp.error());
          // Handle this error more gracefully, perhaps by attempting to reinitialize or exit.
          done = true; // For now, we'll exit.
        }
        if (threeDEngine) {
          threeDEngine->onSwapchainRecreated(wd.getImageCount());
        }
        swapChainRebuild = false;
      }

      ImGui_ImplVulkan_NewFrame();
      ImGui_ImplSDL3_NewFrame();
      ImGui::NewFrame();

      imguiMenu.renderMegaMenu(imguiMenu, wd, device, threeDEngine->getCamera(),
                               threeDEngine->getScene(), textSystem,
                               threeDEngine->getShaderToggles(), threeDEngine->getLightUbo(),
                               textToggles, fontSizeMultiplier, wd.getCurrentFrame(), deltaTime);

      ImGui::Render();
      FrameRender(ImGui::GetDrawData(), deltaTime);
    }
  }

public:
  // The main entry point for the application. It returns an integer status code.
  // 0 for success, non-zero for failure.
  int run(SDL_Window *sdl_window) {
    // We now check the result of each initialization step.
    // If a step fails, we print the error and return a non-zero exit code.
    if (auto res = SetupVulkan(); !res) {
      std::println("Vulkan setup failed: {}", res.error());
      return 1;
    }
    if (auto res = SetupVulkanWindow(sdl_window, get_window_size(sdl_window)); !res) {
      std::println("Vulkan window setup failed: {}", res.error());
      return 1;
    }

    u32 numMeshesEstimate = 100;
    // Assume device.createDescriptorPool also returns std::expected
    auto createDescriptorPoolResult =
        device.createDescriptorPool(static_cast<u32>(wd.getImageCount()) * numMeshesEstimate);
    if (!createDescriptorPoolResult) {
      std::println("Failed to create descriptor pool: {}", createDescriptorPoolResult.error());
      return 1;
    }

    // Initialize Engines
    threeDEngine = std::make_unique<ThreeDEngine>(device, wd.getImageCount(), thread_pool);
    // Assume threeDEngine->initialize also returns std::expected
    if (auto res = threeDEngine->initialize(wd.getRenderPass(), pipelineCache); !res) {
      std::println("3D engine initialization failed: {}", res.error());
      return 1;
    }
    threeDEngine->loadInitialAssets();

    if (auto res = create2DGraphicsPipelines(); !res) {
      std::println("2D graphics pipeline creation failed: {}", res.error());
      return 1;
    }

    auto textSystemExpected =
        TextSystem::create(device, wd.getImageCount(), device.descriptorPool_);
    if (!textSystemExpected) {
      std::println("textSystem creation failed: {}", textSystemExpected.error());
      return 1;
    }
    textSystem = std::move(textSystemExpected.value());

    auto uiSystemExpected = UISystem::create(device, wd.getImageCount(), device.descriptorPool_);
    if (!uiSystemExpected) {
      std::println("uiSystem creation failed: {}", uiSystemExpected.error());
      return 1;
    }
    uiSystem = std::move(uiSystemExpected.value());

    // Load 2D assets
    {
      std::lock_guard lock(fontFuturesMutex);
      fontLoadFutures.emplace_back(thread_pool.submit_task([this]() {
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
        textSetLayout = device.logical().createDescriptorSetLayout(textLayoutInfo).value();
        return textSystem.registerFont(
            "../../assets/fonts/Inconsolata/InconsolataNerdFontMono-Regular.ttf", 64,
            textSetLayout);
      }));
    }

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForVulkan(sdl_window);
    ImGui_ImplVulkan_InitInfo init_info = device.init_info();
    init_info.Instance = instance.get_C_handle();
    init_info.RenderPass = *wd.getRenderPass();
    init_info.MinImageCount = MIN_IMAGE_COUNT;
    init_info.ImageCount = static_cast<u32>(wd.getImageCount());
    init_info.PipelineCache = *pipelineCache;
    init_info.CheckVkResultFn = check_vk_result_for_imgui;
    ImGui_ImplVulkan_Init(&init_info);
    // ImGui_ImplVulkan_CreateFontsTexture();

    mainLoop(sdl_window);

    device.logical().waitIdle();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    return 0; // Success
  }
};
