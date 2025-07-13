module; // Global module fragment

// It's essential to include the original header in the global module fragment.
// If TINYGLTF_IMPLEMENTATION is needed for the functions/data you want to export
// (and they aren't just declarations), it would theoretically go here,
// making this module unit define those symbols. However, for a pure interface,
// you'd compile the implementation separately or link against a lib.
// For header-only, this include brings in all declarations.

// #define TINYGLTF_USE_STB_IMAGE // If you want STB image loading
// #define TINYGLTF_USE_STB_IMAGE_WRITE // If you want STB image writing
// It's often better to compile tinygltf's implementation (with TINYGLTF_IMPLEMENTATION)
// in a separate .cpp file that is part of this module or a linked library.
// For this example, we assume tiny_gltf.h provides the declarations.
#include "tiny_gltf.h" // Or your path to it

export module vulkan_app:tinygltf; // Your module name

// Exporting selected parts of tinygltf
export namespace gltfm {
// --- Core Classes ---
using tinygltf::Accessor;
using tinygltf::Animation;
using tinygltf::AnimationChannel;
using tinygltf::AnimationSampler;
using tinygltf::Asset;
using tinygltf::Buffer;
using tinygltf::BufferView;
using tinygltf::Image;
using tinygltf::Material;
using tinygltf::Mesh;
using tinygltf::Node;
using tinygltf::Primitive;
using tinygltf::Sampler;
using tinygltf::Scene;
using tinygltf::Texture;
using tinygltf::TinyGLTF;
// using tinygltf::AnimationChannelTarget; // This is nested in AnimationChannel
using tinygltf::Camera;
using tinygltf::Light;     // For KHR_lights_punctual
using tinygltf::Parameter; // Older, but might be used
using tinygltf::Skin;
using tinygltf::Value; // For extensions, extras, etc.

// --- Model (often the main container) ---
using tinygltf::Model;

// --- Structs for Material Properties ---
using tinygltf::NormalTextureInfo;
using tinygltf::OcclusionTextureInfo;
using tinygltf::PbrMetallicRoughness;
// using tinygltf::PbrSpecularGlossiness; // If using KHR_materials_pbrSpecularGlossiness
using tinygltf::TextureInfo;
// using tinygltf::MaterialExtension; // Base for material extensions

// --- Structs for Camera Properties ---
using tinygltf::OrthographicCamera;
using tinygltf::PerspectiveCamera;

// --- Structs for Primitive Attributes ---
// `attributes` in `Primitive` is `std::map<std::string, int>`
// You might alias std::string if used heavily for attribute keys.
// using AttributeKey = std::string;

// --- Key Functions (assuming 'static' is removed or they are otherwise made available) ---
// Note: These functions are originally static in tiny_gltf.h.
// You mentioned you'd handle this. If they become non-static members of TinyGLTF,
// you wouldn't need to export them separately here.
// If they become free functions in the tinygltf namespace, then:
// using tinygltf::GetComponentSizeInBytes;
// using tinygltf::GetNumComponentsInType;
// using tinygltf::GetShaderVersion; // If needed
// using tinygltf::IsDataURI;
// using tinygltf::DecodeDataURI;
// For now, let's assume you might make them free functions or directly accessible:
inline int GetComponentSizeInBytes(int componentType) {
  return tinygltf::GetComponentSizeInBytes(componentType);
}
inline int GetNumComponentsInType(int type) { return tinygltf::GetNumComponentsInType(type); }

// --- Enums and Constants (as constexpr or enum class for type safety) ---

// Component Types from tiny_gltf.h (Section 5.1.2)
constexpr int COMPONENT_TYPE_BYTE = TINYGLTF_COMPONENT_TYPE_BYTE;                     // 5120
constexpr int COMPONENT_TYPE_UNSIGNED_BYTE = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;   // 5121
constexpr int COMPONENT_TYPE_SHORT = TINYGLTF_COMPONENT_TYPE_SHORT;                   // 5122
constexpr int COMPONENT_TYPE_UNSIGNED_SHORT = TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT; // 5123
constexpr int COMPONENT_TYPE_INT = TINYGLTF_COMPONENT_TYPE_INT; // (custom, not in spec)
constexpr int COMPONENT_TYPE_UNSIGNED_INT = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT; // 5125
constexpr int COMPONENT_TYPE_FLOAT = TINYGLTF_COMPONENT_TYPE_FLOAT;               // 5126
constexpr int COMPONENT_TYPE_DOUBLE =
    TINYGLTF_COMPONENT_TYPE_DOUBLE; // (custom, not in spec, 5130 in glTF 1.0)

// Type (Number of components) from tiny_gltf.h (Section 5.1.1)
constexpr int TYPE_SCALAR = TINYGLTF_TYPE_SCALAR;
constexpr int TYPE_VEC2 = TINYGLTF_TYPE_VEC2;
constexpr int TYPE_VEC3 = TINYGLTF_TYPE_VEC3;
constexpr int TYPE_VEC4 = TINYGLTF_TYPE_VEC4;
constexpr int TYPE_MAT2 = TINYGLTF_TYPE_MAT2;
constexpr int TYPE_MAT3 = TINYGLTF_TYPE_MAT3;
constexpr int TYPE_MAT4 = TINYGLTF_TYPE_MAT4;
// These are for glTF 1.0 but defined in tinygltf
constexpr int TYPE_VECTOR = TINYGLTF_TYPE_VECTOR;
constexpr int TYPE_MATRIX = TINYGLTF_TYPE_MATRIX;

// Texture Filter Types (Section 5.2.6)
constexpr int TEXTURE_FILTER_NEAREST = TINYGLTF_TEXTURE_FILTER_NEAREST; // 9728
constexpr int TEXTURE_FILTER_LINEAR = TINYGLTF_TEXTURE_FILTER_LINEAR;   // 9729
constexpr int TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST =
    TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST; // 9984
constexpr int TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST =
    TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST; // 9985
constexpr int TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR =
    TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR; // 9986
constexpr int TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR =
    TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR; // 9987

// Texture Wrap Types (Section 5.2.7)
constexpr int TEXTURE_WRAP_REPEAT = TINYGLTF_TEXTURE_WRAP_REPEAT;                   // 10497
constexpr int TEXTURE_WRAP_CLAMP_TO_EDGE = TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;     // 33071
constexpr int TEXTURE_WRAP_MIRRORED_REPEAT = TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT; // 33648

// Primitive Modes (Section 3.8.1.2)
constexpr int PRIMITIVE_MODE_POINTS = TINYGLTF_MODE_POINTS; // 0
// constexpr int PRIMITIVE_MODE_LINES = TINYGLTF_MODE_LINES;                   // 1
constexpr int PRIMITIVE_MODE_LINE_LOOP = TINYGLTF_MODE_LINE_LOOP;           // 2
constexpr int PRIMITIVE_MODE_LINE_STRIP = TINYGLTF_MODE_LINE_STRIP;         // 3
constexpr int PRIMITIVE_MODE_TRIANGLES = TINYGLTF_MODE_TRIANGLES;           // 4
constexpr int PRIMITIVE_MODE_TRIANGLE_STRIP = TINYGLTF_MODE_TRIANGLE_STRIP; // 5
constexpr int PRIMITIVE_MODE_TRIANGLE_FAN = TINYGLTF_MODE_TRIANGLE_FAN;     // 6

// Alpha Modes (Material)
constexpr int MATERIAL_ALPHA_MODE_OPAQUE = 0; // Not a macro in tinygltf, but string "OPAQUE"
constexpr int MATERIAL_ALPHA_MODE_MASK = 1;   // String "MASK"
constexpr int MATERIAL_ALPHA_MODE_BLEND = 2;  // String "BLEND"
// You'd typically compare material.alphaMode == "OPAQUE" etc.
// Or define your own enum class for these and convert from string.
enum class AlphaMode { OPAQUE, MASK, BLEND, UNKNOWN };
inline AlphaMode FromString(const std::string &s) {
  if (s == "OPAQUE")
    return AlphaMode::OPAQUE;
  if (s == "MASK")
    return AlphaMode::MASK;
  if (s == "BLEND")
    return AlphaMode::BLEND;
  return AlphaMode::UNKNOWN;
}

// Common Attribute Name Strings (useful for primitive.attributes.count(...))
// These are not macros in tinygltf, but defined C-strings.
// You can export them as const char* or std::string_view.
// Example:
// inline constexpr const char* ATTRIB_POSITION = "POSITION";
// This is often better handled by just using the strings directly or your own constants.
// For completeness, some are:
// TINYGLTF_ATTRIB_POSITION_STRING
// TINYGLTF_ATTRIB_NORMAL_STRING
// TINYGLTF_ATTRIB_TANGENT_STRING
// TINYGLTF_ATTRIB_TEXCOORD_0_STRING ... TEXCOORD_N_STRING
// TINYGLTF_ATTRIB_COLOR_0_STRING ... COLOR_N_STRING
// TINYGLTF_ATTRIB_JOINTS_0_STRING ... JOINTS_N_STRING
// TINYGLTF_ATTRIB_WEIGHTS_0_STRING ... WEIGHTS_N_STRING

// Image Formats (if you need to check them, though often handled by STB)
// These are not macros, but integer return values from internal functions.
// e.g., tinygltf::TinyGLTF::GetImageDataFormat()
// TINYGLTF_IMAGE_FORMAT_JPEG
// TINYGLTF_IMAGE_FORMAT_PNG
// TINYGLTF_IMAGE_FORMAT_BMP
// TINYGLTF_IMAGE_FORMAT_GIF
// ... and others

// BufferView Target Types (Section 3.7.2.1)
constexpr int ARRAY_BUFFER = TINYGLTF_TARGET_ARRAY_BUFFER;                 // 34962
constexpr int ELEMENT_ARRAY_BUFFER = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER; // 34963

// Camera Types
// const std::string CAMERA_TYPE_PERSPECTIVE = "perspective";
// const std::string CAMERA_TYPE_ORTHOGRAPHIC = "orthographic";
// Similar to AlphaMode, you'd compare camera.type string or convert to an enum.
enum class CameraType { PERSPECTIVE, ORTHOGRAPHIC, UNKNOWN };
inline CameraType CameraTypeFromString(const std::string &s) {
  if (s == "perspective")
    return CameraType::PERSPECTIVE;
  if (s == "orthographic")
    return CameraType::ORTHOGRAPHIC;
  return CameraType::UNKNOWN;
}

// --- Utility for Extensions ---
// The `extensions` and `extras` members in various glTF objects are `Value` types.
// You might want to export common helper functions if you write them.
// e.g., template<typename T> bool TryGetExtensionValue(const tinygltf::Value& extensions, const
// std::string& key, T& outValue);

} // namespace gltfm
