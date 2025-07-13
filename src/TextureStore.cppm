module;

#include "macros.hpp"
#include "primitive_types.hpp"
#include <xxhash.h> // Include the xxHash header

export module vulkan_app:TextureStore;

import vulkan_hpp;
import std;
import :VulkanDevice;
import :texture;
import :ModelLoader; // For GltfImageData

export class TextureStore {
private:
  VulkanDevice &device_;
  const vk::raii::Queue &transferQueue_;

  // The core change: Use a u32 hash as the key for our texture cache.
  std::map<u32, std::shared_ptr<Texture>> loadedTextures_;
  std::shared_ptr<Texture> defaultWhiteTexture_;
  std::shared_ptr<Texture> defaultNormalTexture_;
  std::shared_ptr<Texture> defaultMRTexture_;
  std::shared_ptr<Texture> defaultEmissiveTexture_;

public:
  [[nodiscard]] std::expected<void, std::string> createDefaultTextures() {
    // if (!*device_.logical() || device_.queueFamily_ == static_cast<u32>(-1))
    //   return std::unexpected("TS:createPool: Device/QF not init.");
    //
    // vk::CommandPoolCreateInfo poolInfo{.flags = vk::CommandPoolCreateFlagBits::eTransient |
    //                                             vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
    //                                    .queueFamilyIndex = device_.queueFamily_};
    // auto poolResult = device_.logical().createCommandPool(poolInfo);
    // if (!poolResult)
    //   return std::unexpected("TS:createPool: Pool creation failed: " +
    //                          vk::to_string(poolResult.error()));
    // commandPool_ = std::move(poolResult.value());

    // --- Create the default texture and add it to the cache ---
    // As you mentioned, loading this from a file is a great idea for a real app.
    // For now, we create a 1x1 white texture programmatically and let it be the first
    // entry in our cache.
    std::array<uint8_t, 4> whitePixel = {255, 255, 255, 255};
    defaultWhiteTexture_ = getColorTexture(whitePixel); // This will hash and store it.
    std::array<uint8_t, 4> normalPixel = {128, 128, 128, 255};
    defaultNormalTexture_ = getColorTexture(normalPixel); // This will hash and store it.
    std::array<uint8_t, 4> mrPixel = {0, 255, 0, 255};
    defaultMRTexture_ = getColorTexture(mrPixel); // This will hash and store it.
    std::array<uint8_t, 4> emissivePixel = {0, 0, 0, 255};
    defaultEmissiveTexture_ = getColorTexture(emissivePixel); // This will hash and store it.

    if (!defaultWhiteTexture_ || !defaultNormalTexture_ || !defaultMRTexture_ ||
        !defaultEmissiveTexture_) {
      // The getColorTexture method already prints errors, but we can add a critical one here.
      std::println("CRITICAL: TextureStore failed to create the default white texture.");
      return std::unexpected("Failed to create default white texture.");
    }

    return {};
  }

  TextureStore(VulkanDevice &device, const vk::raii::Queue &queue)
      : device_(device), transferQueue_(queue) {}

  // Load texture from raw pixel data using its hash as the key.
  [[nodiscard]] std::shared_ptr<Texture> getTextureFromData(const GltfImageData &imageData) {
    if (imageData.pixels.empty() || imageData.width == 0 || imageData.height == 0) {
      std::println("TS::getTextureFromData: Invalid image data. Returning fallback default.");
      return defaultWhiteTexture_;
    }

    // --- HASHING LOGIC ---
    // Generate a 32-bit hash from the pixel data.
    const u32 textureHash = XXH32(imageData.pixels.data(), imageData.pixels.size(), 0);

    if (auto it = loadedTextures_.find(textureHash); it != loadedTextures_.end()) {
      std::println("texture already exists {}", imageData.name);
      return it->second; // Texture already exists, return it.
    }

    // --- FORMAT SELECTION (same as before) ---
    vk::Format format = vk::Format::eR8G8B8A8Unorm; // Default
    if (imageData.component == 4) {
      format = imageData.isSrgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
    } else {
      // Simplified logic: Warn if not 4 components, but proceed assuming the createTexture
      // function or the data itself is prepared for an RGBA format. Proper handling
      // for 3-component (RGB) images would typically involve converting them to RGBA
      // before hashing and uploading.
      std::println("Warning: Texture with hash {:#x} has {} components. Assuming RGBA for format.",
                   textureHash, imageData.component);
      format = imageData.isSrgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
    }

    // --- TEXTURE CREATION ---
    auto texResult = createTexture( // This is your existing createTexture from vulkan_app:texture
        device_, imageData.pixels.data(), imageData.pixels.size(),
        vk::Extent3D{.width = static_cast<u32>(imageData.width),
                     .height = static_cast<u32>(imageData.height),
                     .depth = 1},
        format, transferQueue_,
        true // generateMipmaps
    );

    if (texResult) {
      auto newTexture = std::make_shared<Texture>(std::move(*texResult));
      loadedTextures_[textureHash] = newTexture; // Store using the hash
      std::println("TextureStore: Created new texture with hash {:#x}.", textureHash);
      return newTexture;
    }

    std::println("TextureStore: Failed to create texture with hash {:#x}: {}. Returning fallback.",
                 textureHash, texResult.error());
    return defaultWhiteTexture_;
  }

  // Get or create a solid-color texture. The key is now the hash of the color itself.
  [[nodiscard]] std::shared_ptr<Texture>
  getColorTexture(const std::array<uint8_t, 4> &color,
                  vk::Format format = vk::Format::eR8G8B8A8Srgb) {
    // --- HASHING LOGIC for color ---
    const u32 colorHash = XXH32(color.data(), color.size(), 0);

    if (auto it = loadedTextures_.find(colorHash); it != loadedTextures_.end()) {
      return it->second;
    }

    // Create a 1x1 texture for the solid color
    auto texResult =
        createTexture(device_, color.data(), color.size(), {.width = 1, .height = 1, .depth = 1},
                      format, transferQueue_, false);
    if (texResult) {
      auto newTex = std::make_shared<Texture>(std::move(*texResult));
      loadedTextures_[colorHash] = newTex;
      return newTex;
    }
    std::println("TS: Failed to create color texture with hash {:#x}: {}. Returning fallback.",
                 colorHash, texResult.error());
    return defaultWhiteTexture_;
  }

  [[nodiscard]] std::shared_ptr<Texture> getDefaultTexture() const { return defaultWhiteTexture_; }
  [[nodiscard]] std::shared_ptr<Texture> getDefaultNormalTexture() const {
    return defaultNormalTexture_;
  }
  [[nodiscard]] std::shared_ptr<Texture> getDefaultMRTexture() const { return defaultMRTexture_; }
  [[nodiscard]] std::shared_ptr<Texture> getDefaultEmissiveTexture() const {
    return defaultEmissiveTexture_;
  }
  [[nodiscard]] PBRTextures getAllDefaultTextures() const {
    return PBRTextures{
        .baseColor = defaultWhiteTexture_,
        .metallicRoughness = defaultMRTexture_,
        .normal = defaultWhiteTexture_,
        .occlusion = defaultWhiteTexture_,
        .emissive = defaultEmissiveTexture_,
    };
  };

  ~TextureStore() = default;
  TextureStore(const TextureStore &) = delete;
  TextureStore &operator=(const TextureStore &) = delete;
  TextureStore(TextureStore &&) = delete;
  TextureStore &operator=(TextureStore &&) = delete;
};
