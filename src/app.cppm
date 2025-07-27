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

import :InputHandler;
import :VulkanWindow;
import :VulkanDevice;
import :VulkanInstance;
import :VulkanPipeline;
import :utils;
import :imgui;
import :TextSystem;
import :UISystem;
import :ui;
import :TextEditor;
import :TextView;
import :TwoDEngine;   // Import the new 3D engine module
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
  VulkanDevice m_device;
  VulkanWindow m_wd;
  bool m_swapChainRebuild = false;

  std::unique_ptr<ThreeDEngine> m_threeDEngine;
  std::unique_ptr<TwoDEngine> m_twoDEngine;

  vk::raii::PipelineCache m_pipelineCache{nullptr};

  InputHandler m_inputHandler{nullptr, nullptr};

  BS::thread_pool<> m_thread_pool;

  ImGuiMenu m_imguiMenu;

private:
  [[nodiscard]] std::expected<void, std::string> setupVulkan() {
    // Each of these methods in the classes `VulkanInstance` and `VulkanDevice`
    // should also be refactored to return std::expected instead of calling std::exit.
    if (auto res = m_instance.create(); !res) {
      return std::unexpected(res.error());
    }
    if (auto res = m_device.create(m_instance); !res) {
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

      if (m_twoDEngine) {
        m_twoDEngine->update(m_wd.getImageIndex(), deltaTime, m_wd.getExtent());
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

      if (m_twoDEngine) {
        m_twoDEngine->draw(cmd, m_wd.getImageIndex(), m_wd.getExtent());
      }

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
        // Give ImGui the first chance to process the event.
        // This will update its internal state, like `WantCaptureKeyboard`.
        ImGui_ImplSDL3_ProcessEvent(&event);

        // NEW: If ImGui is not using the keyboard, pass the event to our handler.
        if (!imguiIO.WantCaptureKeyboard) {
          m_inputHandler.handleEvent(event);
        }

        if (m_inputHandler.shouldCycleFocus()) {
          m_twoDEngine->cycleActiveWidgetFocus();
          m_inputHandler.setEditor(m_twoDEngine->getActiveTextEditor());
          m_inputHandler.resetCycleFocusFlag();
        }

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
        // if (event.type == SDL_EVENT_MOUSE_WHEEL && m_textView) {
        //   m_textView->scroll(event.wheel.y > 0 ? -3 : 3);
        // }
      }

      if (m_threeDEngine) {
        m_threeDEngine->processGltfLoads();
      }

      if (m_twoDEngine) {
        m_twoDEngine->processFontLoads();
      }
      // // NEW: Only allow camera movement if not in INSERT mode.
      // // This prevents typing 'w' from moving the camera.
      // if (m_threeDEngine && m_inputHandler.getMode() != InputMode::INSERT) {
      //   readKeyboard(deltaTime, sdlWindow);
      // }

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
        if (m_twoDEngine) {
          m_twoDEngine->onSwapchainRecreated(m_wd.getImageCount());
        }
        m_swapChainRebuild = false;
      }

      ImGui_ImplVulkan_NewFrame();
      ImGui_ImplSDL3_NewFrame();
      ImGui::NewFrame();

      m_imguiMenu.renderMegaMenu(m_imguiMenu, m_wd, m_device, m_threeDEngine->getCamera(),
                                 m_threeDEngine->getScene(), m_threeDEngine->getShaderToggles(),
                                 m_threeDEngine->getLightUbo(), m_twoDEngine->textToggles,
                                 m_twoDEngine->fontSizeMultiplier, m_wd.getCurrentFrame(),
                                 deltaTime);

      // NEW: Render a status window to show the current input mode.
      {
        ImGui::Begin("Status");
        const char *modeText =
            (m_inputHandler.getMode() == InputMode::INSERT) ? "INSERT" : "NORMAL";
        ImGui::Text("Input Mode: %s", modeText);
        ImGui::Text("Press 'i' for Insert Mode, 'Esc' for Normal Mode.");
        ImGui::End();
      }

      ImGui::Render();
      frameRender(ImGui::GetDrawData(), deltaTime);
    }
  }

public:
  // The main entry point for the application. It returns an integer status code.
  // 0 for success, non-zero for failure.
  int run(SDL_Window *sdlWindow) {
    m_inputHandler.setSDLwindow(sdlWindow);
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

    m_twoDEngine = std::make_unique<TwoDEngine>(m_device, m_wd.getImageCount(), m_thread_pool);
    if (auto res = m_twoDEngine->initialize(m_wd.getRenderPass(), m_pipelineCache); !res) {
      std::println("2D engine initialization failed: {}", res.error());
      return 1;
    }
    m_inputHandler.setEditor(m_twoDEngine->getActiveTextEditor());

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
