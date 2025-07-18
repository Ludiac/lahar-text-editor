import vulkan_app;

int main() {
  SdlWrapper sdl;
  sdl.init();
  {
    App app;
    app.run(sdl.window);
  }
  sdl.terminate();
  return 0;
}
