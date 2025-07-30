module;

#include "macros.hpp"
#include "primitive_types.hpp"
#include <msdf-atlas-gen/msdf-atlas-gen.h> // Include for msdf_atlas namespace
// #include <msdfgen-ext.h>    // For Freetype integration
// #include <msdfgen.h>

export module vulkan_app:text;

import :texture;
import vulkan_hpp;
import std;

// A simple RAII/scope guard helper for automatic cleanup.
namespace {
template <typename F> struct ScopeGuard {
  F f;
  ScopeGuard(F &&f) : f(std::move(f)) {}
  ~ScopeGuard() { f(); }
};
} // namespace

// This structure holds all the information needed to render a single character.
export struct GlyphInfo {
  // Texture coordinates for the glyph in the atlas
  f64 uvX0, uvY0, uvX1, uvY1;

  // Size of the glyph quad in pixels
  f64 width, height;

  // The offset from the text cursor's baseline to the glyph's top-left corner
  f64 bearingX, bearingY;

  // The horizontal distance to advance the cursor to the next character
  f64 advance;
};

// This struct holds the complete output of the font atlas generation.
export struct FontAtlasData {
  // The atlas is now multi-channel (4 channels for MTSDF)
  std::vector<msdf_atlas::byte> atlasBitmap;
  int atlasWidth{};
  int atlasHeight{};
  std::map<msdfgen::unicode_t, GlyphInfo> glyphs;
  f64 pxRange{};    // Store the pixel range for the shader
  f64 atlasScale{}; // **NEW**: Store the scale used by the atlas packer

  f64 unitsPerEm{}; // Font design units per EM
  f64 ascender{};   // Distance from baseline to highest point
  f64 descender{};  // Distance from baseline to lowest point
  f64 lineHeight{}; // Recommended line spacing
};

export struct Font {
  FontAtlasData atlasData;
  std::shared_ptr<Texture> texture;
  vk::raii::DescriptorSet textureDescriptorSet{nullptr};
};

/**
 * @brief Generates a Multi-channel True Signed Distance Field (MTSDF) font
 * atlas. This function uses msdf-gen to create a high-quality,
 * resolution-independent font atlas from a TTF file. The RGB channels store the
 * MSDF, and the Alpha channel stores a true SDF.
 *
 * @param fontPath Path to the .ttf font file.
 * calculations.
 * @return A FontAtlasData struct containing the MTSDF bitmap, dimensions, and
 * glyph metrics.
 */
export [[nodiscard]] std::expected<FontAtlasData, std::string>
createFontAtlasMSDF(const std::string &fontPath) {
  FontAtlasData atlasData;

  msdfgen::FreetypeHandle *ftHandle = msdfgen::initializeFreetype();
  if (ftHandle == nullptr) {
    return std::unexpected("MSDFGEN: Failed to initialize Freetype handle");
  }
  ScopeGuard ftHandleGuard([&]() { msdfgen::deinitializeFreetype(ftHandle); });

  msdfgen::FontHandle *font = msdfgen::loadFont(ftHandle, fontPath.c_str());
  if (font == nullptr) {
    return std::unexpected("MSDFGEN: Failed to load font handle");
  }
  ScopeGuard fontGuard([&]() { msdfgen::destroyFont(font); });

  // Get font metrics
  msdfgen::FontMetrics metrics{};
  if (!msdfgen::getFontMetrics(metrics, font, msdfgen::FONT_SCALING_NONE)) {
    return std::unexpected("Failed to get font metrics");
  }

  atlasData.unitsPerEm = metrics.emSize;
  atlasData.ascender = metrics.ascenderY;
  atlasData.descender = metrics.descenderY;
  atlasData.lineHeight = metrics.lineHeight;

  // --- Configuration ---
  const f64 ANGLE_THRESHOLD = 3.0;
  const f64 MITER_LIMIT = 1.0;
  atlasData.pxRange = atlasData.unitsPerEm / 64; // The distance field range in atlas pixels.
  // atlasData.pxRange = 2.0;

  const std::string FULL_CHARSET = " ?"
                                   "abcdefghiklmnoprstuv";
  std::string currentCharset = "Vulkan?";
  if (currentCharset.empty()) {
    currentCharset = FULL_CHARSET;
  }
  currentCharset = FULL_CHARSET;

  std::vector<msdf_atlas::GlyphGeometry> glyphs;

  for (char cChar : currentCharset) {
    auto c = static_cast<msdfgen::unicode_t>(cChar);
    msdf_atlas::GlyphGeometry glyphGeometry;
    const f64 GEOMETRY_SCALE = 1.0; // This is a typical value, adjust if needed
    if (!glyphGeometry.load(font, GEOMETRY_SCALE, c, true)) {
      std::cerr << "Warning: Could not load glyph geometry for character '" << cChar << "'\n";
      continue;
    }
    glyphs.emplace_back(glyphGeometry); // Use move to avoid copy if GlyphGeometry is large
  }
  // alternative, but buggy
  // msdf_atlas::FontGeometry fontGEometry;
  // msdf_atlas::Charset charset;
  // for (char c_char : currentCharset) {
  //   charset.add(static_cast<msdfgen::unicode_t>(c_char));
  // }
  // f64 geometryScale = 1.0; // Use a scale of 1.0 for now, the packer will
  // handle the final scale int num = fontGEometry.loadCharset(font,
  // geometryScale, charset); auto frfr = fontGEometry.getGlyphs();
  // glyphs.assign(frfr.begin(), frfr.end());
  std::println("DEBUG: Loaded {} glyphs from font.", glyphs.size());

  if (glyphs.empty()) {
    return std::unexpected("MSDFGEN: No glyphs loaded from font. Check font file and charset.");
  }

  // Apply edge coloring to all loaded glyphs
  for (msdf_atlas::GlyphGeometry &glyph : glyphs) {
    glyph.edgeColoring(msdfgen::edgeColoringSimple, ANGLE_THRESHOLD, 0);
  }

  // --- Atlas Packing ---
  msdf_atlas::TightAtlasPacker packer;
  packer.setPixelRange(atlasData.pxRange);
  // Let the packer determine the scale automatically based on glyphs and pixel
  // range Or uncomment the next line to force a specific pixel size for the EM
  packer.setMiterLimit(MITER_LIMIT);
  packer.setOuterUnitPadding(32);
  // packer.setScale(1.0);
  packer.pack(glyphs.data(), glyphs.size());

  int width = 0;
  int height = 0;
  packer.getDimensions(width, height);
  atlasData.atlasWidth = width;
  atlasData.atlasHeight = height;

  atlasData.atlasScale = packer.getScale();
  std::println("DEBUG: Atlas Packer Scale = {}", atlasData.atlasScale);
  std::println("DEBUG: Atlas Dimensions after packing: Width = {}, Height = {}", width, height);
  if (width <= 0 || height <= 0) {
    return std::unexpected(std::format("MSDF-ATLAS-GEN: Failed to pack symbols. Atlas dimensions "
                                       "are invalid: w={}, h={}",
                                       width, height));
  }

  // --- Atlas Generation (Using MTSDF) ---
  // This generator produces MSDF in RGB and a true SDF in the Alpha channel.
  msdf_atlas::GeneratorAttributes attributes;
  attributes.config.overlapSupport = true;
  attributes.scanlinePass = true;

  // Use the mtsdfGenerator with 4 channels (byte-based)
  msdf_atlas::ImmediateAtlasGenerator<float, // Intermediate format for processing
                                      4,     // 4 output channels (RGBA)
                                      msdf_atlas::mtsdfGenerator, // The generator for MSDF+SDF
                                      msdf_atlas::BitmapAtlasStorage<msdf_atlas::byte, 4> // Final
                                                                                          // storage
                                                                                          // format
                                      >
      generator(width, height);

  generator.setAttributes(attributes);
  generator.setThreadCount(12);
  std::println("DEBUG: Generating MTSDF atlas bitmap...");
  generator.generate(glyphs.data(), glyphs.size());
  std::println("DEBUG: Atlas bitmap generation complete.");

  msdfgen::BitmapConstRef<msdf_atlas::byte, 4> bitmap = generator.atlasStorage();
  atlasData.atlasBitmap.assign(
      bitmap.pixels, static_cast<ptrdiff_t>(bitmap.width * bitmap.height * 4) + bitmap.pixels);

  // --- Store Glyph Metrics ---
  std::println("DEBUG: Glyph metrics stored. Total glyphs: {}", glyphs.size());
  for (const auto &glyph : glyphs) {
    f64 l;
    f64 b;
    f64 r;
    f64 t;
    glyph.getQuadPlaneBounds(l, b, r, t);

    f64 u;
    f64 v;
    f64 s;
    f64 w;
    glyph.getQuadAtlasBounds(u, v, s, w);

    GlyphInfo info{};
    // Normalize UVs to [0, 1] range
    info.uvX0 = u / atlasData.atlasWidth;
    info.uvY0 = v / atlasData.atlasHeight;
    info.uvX1 = s / atlasData.atlasWidth;
    info.uvY1 = w / atlasData.atlasHeight;

    // Store glyph plane dimensions in font units
    info.width = r - l;
    info.height = t - b;

    // Store bearing in font units
    info.bearingX = l;
    info.bearingY = t;

    // Store advance in font units
    info.advance = glyph.getAdvance();

    atlasData.glyphs[glyph.getCodepoint()] = info;
  }
  std::println("DEBUG: Glyph metrics stored. Total glyphs in map: {}", atlasData.glyphs.size());

  return atlasData;
}
