module;

#include "macros.hpp"
#include "primitive_types.hpp"
#include <SDL3/SDL.h>

export module vulkan_app:SDLWrapper;

import :Types;
import vulkan_hpp;
import std;

export struct SDL_Wrapper {
  SDL_Window *window{nullptr};

  int init() {
    if constexpr (VK_USE_PLATFORM_WAYLAND_KHR) {
      SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      std::cerr << "[SDL_Wrapper] Error: SDL_Init(): " << SDL_GetError() << std::endl;

      return -1;
    }
    SDL_WindowFlags window_flags =
        (SDL_WindowFlags)(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    window = SDL_CreateWindow("Dear ImGui SDL3+Vulkan example", 1280, 720, window_flags);
    if (window == nullptr) {
      std::cerr << "[SDL_Wrapper] Error: SDL_CreateWindow(): " << SDL_GetError() << std::endl;

      return -1;
    }
    return 0;
  }

  void terminate() {
    if (window) {
      SDL_DestroyWindow(window);
      window = nullptr;
    }
    SDL_PumpEvents(); // Process any pending events
    SDL_Quit();
  }
};
