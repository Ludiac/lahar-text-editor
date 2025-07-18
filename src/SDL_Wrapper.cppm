module;

#include "macros.hpp"
#include "primitive_types.hpp"
#include <SDL3/SDL.h>

export module vulkan_app:SDLWrapper;

import :Types;
import vulkan_hpp;
import std;

export struct SdlWrapper {
  SDL_Window *window{nullptr};

  int init() {
    if constexpr (VK_USE_PLATFORM_WAYLAND_KHR) {
      SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      std::cerr << "[SDL_Wrapper] Error: SDL_Init(): " << SDL_GetError() << '\n';

      return -1;
    }
    auto windowFlags =
        (SDL_WindowFlags)(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    window = SDL_CreateWindow("Dear ImGui SDL3+Vulkan example", 1280, 720, windowFlags);
    if (window == nullptr) {
      std::cerr << "[SDL_Wrapper] Error: SDL_CreateWindow(): " << SDL_GetError() << '\n';

      return -1;
    }
    return 0;
  }

  void terminate() {
    if (window != nullptr) {
      SDL_DestroyWindow(window);
      window = nullptr;
    }
    SDL_PumpEvents(); // Process any pending events
    SDL_Quit();
  }
};
