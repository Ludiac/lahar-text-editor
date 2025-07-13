module;

#define GLM_ENABLE_EXPERIMENTAL
#include "macros.hpp"
#include "primitive_types.hpp"
// #include "tiny_gltf.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/norm.hpp> // For length2

export module vulkan_app:ModelLoader;

import std;
import :utils;
import :mesh;
import :texture;
import :tinygltf;

// Structure to hold data for a single GLTF primitive
export struct GltfPrimitiveData {
  std::vector<Vertex> vertices;
  std::vector<u32> indices;
  Material material;

  int baseColorTextureGltfIndex = -1;
  int metallicRoughnessTextureGltfIndex = -1;
  int normalTextureGltfIndex = -1;
  int occlusionTextureGltfIndex = -1;
  int emissiveTextureGltfIndex = -1;
  int transmissionTextureGltfIndex;
  int clearcoatNormalTextureGltfIndex;

  int gltfMaterialIndex = -1;
};

// Structure to hold data for a single GLTF mesh
export struct GltfMeshData {
  std::string name;
  std::vector<GltfPrimitiveData> primitives;
};

// Structure to hold data for a GLTF node
export struct GltfNodeData {
  std::string name;
  glm::mat4 transform{1.0};
  int meshIndex = -1;
  std::vector<int> childrenIndices;
  int parentIndex = -1;
};

// New struct to hold image data loaded by ModelLoader
export struct GltfImageData {
  std::vector<unsigned char> pixels;
  int width = 0;
  int height = 0;
  int component = 0;  // 3 for RGB, 4 for RGBA
  bool isSrgb = true; // Assume sRGB for color textures unless specified otherwise
  std::string name;   // From gltfImage.name or uri
};

export struct LoadedGltfScene {
  std::vector<GltfMeshData> meshes;
  std::vector<GltfNodeData> nodes;
  std::vector<int> rootNodeIndices;

  // Store loaded image data here, indexed by the GLTF image index
  std::map<int, GltfImageData> images;
  // Store GLTF material definitions if needed for more complex mapping later
  // std::vector<gltfm::Material> gltfMaterials;

  std::string error;
};

namespace GltfLoaderHelpers {
// ... (getAccessorData, getIndexAccessorData helpers remain the same as before) ...
template <typename T_Component, int N_Components>
[[nodiscard]] std::expected<std::vector<glm::vec<N_Components, T_Component, glm::defaultp>>,
                            std::string>
getAccessorData(const gltfm::Model &model, int accessorIndex) {
  if (accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= model.accessors.size()) {
    return std::unexpected("Accessor index out of bounds.");
  }
  const gltfm::Accessor &accessor = model.accessors[accessorIndex];
  const gltfm::BufferView &bufferView = model.bufferViews[accessor.bufferView];
  const gltfm::Buffer &buffer = model.buffers[bufferView.buffer];
  std::vector<glm::vec<N_Components, T_Component, glm::defaultp>> data(accessor.count);
  const unsigned char *bufferData =
      buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
  size_t componentSize = gltfm::GetComponentSizeInBytes(accessor.componentType);
  size_t numComponentsPerElement = gltfm::GetNumComponentsInType(accessor.type);

  if (numComponentsPerElement != N_Components) {
    return std::unexpected("Accessor component count mismatch.");
  }

  size_t stride = accessor.ByteStride(bufferView);
  if (stride == 0) {
    stride = componentSize * numComponentsPerElement;
  }

  for (size_t i = 0; i < accessor.count; ++i) {
    const unsigned char *elementData = bufferData + (i * stride);
    if (accessor.componentType == gltfm::COMPONENT_TYPE_FLOAT &&
        sizeof(T_Component) == sizeof(float)) {
      std::memcpy(glm::value_ptr(data[i]), elementData, sizeof(T_Component) * N_Components);
    } else {
      // Simplified: Add more robust type handling/conversion here
      if (sizeof(glm::vec<N_Components, T_Component, glm::defaultp>) ==
          componentSize * numComponentsPerElement) {
        std::memcpy(glm::value_ptr(data[i]), elementData, sizeof(T_Component) * N_Components);
      } else {
        return std::unexpected("Unhandled component type or size mismatch in getAccessorData.");
      }
    }
  }
  return data;
}

[[nodiscard]] std::expected<std::vector<u32>, std::string>
getIndexAccessorData(const gltfm::Model &model, int accessorIndex) {
  if (accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= model.accessors.size()) {
    return std::unexpected("Index accessor index out of bounds.");
  }
  const gltfm::Accessor &accessor = model.accessors[accessorIndex];
  if (accessor.type != gltfm::TYPE_SCALAR) {
    return std::unexpected("Index accessor is not scalar.");
  }
  const gltfm::BufferView &bufferView = model.bufferViews[accessor.bufferView];
  const gltfm::Buffer &buffer = model.buffers[bufferView.buffer];
  const unsigned char *bufferData =
      buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
  std::vector<u32> indices(accessor.count);
  size_t stride = accessor.ByteStride(bufferView);
  if (stride == 0) {
    stride = gltfm::GetComponentSizeInBytes(accessor.componentType);
  }

  for (size_t i = 0; i < accessor.count; ++i) {
    const unsigned char *elementData = bufferData + (i * stride);
    if (accessor.componentType == gltfm::COMPONENT_TYPE_UNSIGNED_INT) {
      u32 val;
      std::memcpy(&val, elementData, sizeof(u32));
      indices[i] = val;
    } else if (accessor.componentType == gltfm::COMPONENT_TYPE_UNSIGNED_SHORT) {
      uint16_t val;
      std::memcpy(&val, elementData, sizeof(uint16_t));
      indices[i] = static_cast<u32>(val);
    } else if (accessor.componentType == gltfm::COMPONENT_TYPE_UNSIGNED_BYTE) {
      uint8_t val;
      std::memcpy(&val, elementData, sizeof(uint8_t));
      indices[i] = static_cast<u32>(val);
    } else {
      return std::unexpected("Unsupported index component type.");
    }
  }
  return indices;
}

void generateTangents(std::vector<Vertex> &vertices, const std::vector<u32> &indices) {
  if (indices.empty()) {
    return;
  }

  std::vector<glm::vec3> temp_tangents(vertices.size(), glm::vec3(0.0));
  std::vector<glm::vec3> temp_bitangents(vertices.size(), glm::vec3(0.0));

  for (size_t i = 0; i < indices.size(); i += 3) {
    Vertex &vertex0 = vertices[indices[i + 0]];
    Vertex &vertex1 = vertices[indices[i + 1]];
    Vertex &vertex2 = vertices[indices[i + 2]];

    glm::vec3 edge1 = vertex1.pos - vertex0.pos;
    glm::vec3 edge2 = vertex2.pos - vertex0.pos;

    glm::vec2 deltaUV1 = vertex1.uv - vertex0.uv;
    glm::vec2 deltaUV2 = vertex2.uv - vertex0.uv;

    float factor = 1.0 / (deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y);
    if (std::isinf(factor) || std::isnan(factor)) {
      continue;
    }

    glm::vec3 tangent;
    tangent.x = factor * (deltaUV2.y * edge1.x - deltaUV1.y * edge2.x);
    tangent.y = factor * (deltaUV2.y * edge1.y - deltaUV1.y * edge2.y);
    tangent.z = factor * (deltaUV2.y * edge1.z - deltaUV1.y * edge2.z);

    glm::vec3 bitangent;
    bitangent.x = factor * (-deltaUV2.x * edge1.x + deltaUV1.x * edge2.x);
    bitangent.y = factor * (-deltaUV2.x * edge1.y + deltaUV1.x * edge2.y);
    bitangent.z = factor * (-deltaUV2.x * edge1.z + deltaUV1.x * edge2.z);

    temp_tangents[indices[i + 0]] += tangent;
    temp_tangents[indices[i + 1]] += tangent;
    temp_tangents[indices[i + 2]] += tangent;

    temp_bitangents[indices[i + 0]] += bitangent;
    temp_bitangents[indices[i + 1]] += bitangent;
    temp_bitangents[indices[i + 2]] += bitangent;
  }

  // MODIFIED: More robust finalization of tangents
  for (size_t i = 0; i < vertices.size(); ++i) {
    Vertex &currentVertex = vertices[i];
    const glm::vec3 &normal = currentVertex.normal;
    const glm::vec3 &t_accum = temp_tangents[i];
    const glm::vec3 &b_accum = temp_bitangents[i];

    // Gram-Schmidt orthogonalize: T' = normalize(T - (T . N) * N)
    // glm::vec3 tangent = t_accum - normal * glm::dot(normal, t_accum);

    // Check for degenerate cases (tangent parallel to normal)
    // if (glm::length2(tangent) > 1e-6f) { // Use length squared for efficiency and robustness
    // tangent = glm::normalize(tangent);
    //
    // // Calculate handedness to handle mirrored UVs
    // float handedness = (glm::dot(glm::cross(n, tangent), b_accum) < 0.0f) ? -1.0f : 1.0f;
    // v.tangent = glm::vec4(tangent, handedness);
    // } else {
    //   // Fallback for when tangent is zero or too small.
    //   // Create an arbitrary but valid tangent.
    //   // This is a common technique using the cross product to find a perpendicular vector.
    glm::vec3 axis =
        (std::abs(normal.x) > std::abs(normal.z)) ? glm::vec3(0.0, 1.0, 0.0) : glm::vec3(1.0, 0.0, 0.0);
    glm::vec3 fallback_t = glm::normalize(glm::cross(normal, axis));
    currentVertex.tangent = glm::vec4(fallback_t, 1.0);
    // }
  }
}
} // namespace GltfLoaderHelpers

export [[nodiscard]] std::expected<LoadedGltfScene, std::string>
loadGltfFile(const std::string &filePath, const std::string &baseDir = "") {
  gltfm::Model model;
  gltfm::TinyGLTF loader;
  std::string err;
  std::string warn;
  LoadedGltfScene loadedScene;

  bool ret;
  if (filePath.size() > 4 && filePath.substr(filePath.size() - 4) == ".glb") {
    ret = loader.LoadBinaryFromFile(&model, &err, &warn, filePath);
  } else {
    ret = loader.LoadASCIIFromFile(&model, &err, &warn, filePath);
  }

  if (!warn.empty()) {
    std::println("TinyGLTF Warning for '{}': {}", filePath, warn);
  }
  if (!err.empty()) {
    return std::unexpected("TinyGLTF Error for '" + filePath + "': " + err);
  }
  if (!ret) {
    return std::unexpected("Failed to parse GLTF file: " + filePath);
  }

  // --- 0. Load Images ---
  for (size_t i = 0; i < model.images.size(); ++i) {
    const auto &gltfImage = model.images[i];
    GltfImageData imageData;
    if (gltfImage.name.empty()) {
        if (gltfImage.uri.empty()) {
            imageData.name = "Image_" + std::to_string(i);
        } else {
            imageData.name = gltfImage.uri;
        }
    } else {
        imageData.name = gltfImage.name;
    }

    if (!gltfImage.uri.empty() && gltfImage.bufferView == -1) { // External image
      std::string imagePath = baseDir.empty() ? gltfImage.uri : baseDir + "/" + gltfImage.uri;
      if (!gltfImage.image.empty()) {
        imageData.pixels = gltfImage.image;
        imageData.width = gltfImage.width;
        imageData.height = gltfImage.height;
        imageData.component = gltfImage.component;
      } else {
        std::println(
            "Warning: GLTF image '{}' has URI but no pixel data loaded by tinygltf. Path: {}",
            gltfImage.name, imagePath);
        continue;
      }
    } else if (gltfImage.bufferView >= 0) { // Embedded image
      const auto &bufferView = model.bufferViews[gltfImage.bufferView];
      const auto &buffer = model.buffers[bufferView.buffer];
      // const unsigned char *data_ptr = buffer.data.data() + bufferView.byteOffset;
      // size_t data_len = bufferView.byteLength;

      if (!gltfImage.image.empty()) {
        imageData.pixels = gltfImage.image;
        imageData.width = gltfImage.width;
        imageData.height = gltfImage.height;
        imageData.component = gltfImage.component;
      } else {
        std::println("Warning: GLTF image '{}' is embedded (bufferView {}) but not decoded by "
                     "tinygltf. MimeType: {}",
                     gltfImage.name, gltfImage.bufferView, gltfImage.mimeType);
        continue;
      }
    } else {
      std::println("Warning: GLTF image '{}' has no URI and no bufferView.", gltfImage.name);
      continue;
    }

    // Color space determination would go here
    imageData.isSrgb = true;

    loadedScene.images[static_cast<int>(i)] = std::move(imageData);
  }

  // 1. Process Meshes and their Primitives
  for (const auto &gltfMesh : model.meshes) {
    GltfMeshData meshData;
    meshData.name = gltfMesh.name.empty() ? ("Mesh_" + std::to_string(loadedScene.meshes.size()))
                                          : gltfMesh.name;

    for (const auto &gltfPrimitive : gltfMesh.primitives) {
      GltfPrimitiveData primitiveData;
      primitiveData.gltfMaterialIndex = gltfPrimitive.material;

      if (gltfPrimitive.indices >= 0) {
        auto indicesResult = GltfLoaderHelpers::getIndexAccessorData(model, gltfPrimitive.indices);
        if (indicesResult) {
          primitiveData.indices = std::move(*indicesResult);
        } else {
          return std::unexpected("Indices load fail: " + indicesResult.error());
        }
      } else {
        return std::unexpected("Non-indexed primitive.");
      }

      std::vector<glm::vec3> positions;
      std::vector<glm::vec3> normals;
      std::vector<glm::vec2> uvs;
      std::vector<glm::vec4> tangents; // To store loaded tangents

      if (auto positionAttributeIt = gltfPrimitive.attributes.find("POSITION");
          positionAttributeIt != gltfPrimitive.attributes.end()) {
        auto res = GltfLoaderHelpers::getAccessorData<float, 3>(model, positionAttributeIt->second);
        if (res) {
          positions = std::move(*res);
        } else {
          return std::unexpected("POSITION: " + res.error());
        }
      } else {
        return std::unexpected("No POSITION attribute.");
      }

      if (auto normalAttributeIt = gltfPrimitive.attributes.find("NORMAL");
          normalAttributeIt != gltfPrimitive.attributes.end()) {
        auto res = GltfLoaderHelpers::getAccessorData<float, 3>(model, normalAttributeIt->second);
        if (res) {
          normals = std::move(*res);
        } else {
          std::println("Warning: Failed to load NORMAL for {}.", meshData.name);
        }
      }
      if (auto texCoordAttributeIt = gltfPrimitive.attributes.find("TEXCOORD_0");
          texCoordAttributeIt != gltfPrimitive.attributes.end()) {
        auto res = GltfLoaderHelpers::getAccessorData<float, 2>(model, texCoordAttributeIt->second);
        if (res) {
          uvs = std::move(*res);
        } else {
          std::println("Warning: Failed to load TEXCOORD_0 for {}.", meshData.name);
        }
      }

      bool hasTangents = false;
      if (auto tangentAttributeIt = gltfPrimitive.attributes.find("TANGENT");
          tangentAttributeIt != gltfPrimitive.attributes.end()) {
        auto res = GltfLoaderHelpers::getAccessorData<float, 4>(model, tangentAttributeIt->second);
        if (res) {
          tangents = std::move(*res);
          hasTangents = true;
        } else {
          std::println("Warning: Failed to load TANGENT for {}.", meshData.name);
        }
      }

      size_t vertexCount = positions.size();
      primitiveData.vertices.resize(vertexCount);
      for (size_t i = 0; i < vertexCount; ++i) {
        primitiveData.vertices[i].pos = positions[i];
        primitiveData.vertices[i].normal = (i < normals.size()) ? normals[i] : glm::vec3(0, 1, 0);
        primitiveData.vertices[i].uv = (i < uvs.size()) ? uvs[i] : glm::vec2(0, 0);
        if (hasTangents && i < tangents.size()) {
          primitiveData.vertices[i].tangent = tangents[i];
        }
      }

      if (!hasTangents) {
        GltfLoaderHelpers::generateTangents(primitiveData.vertices, primitiveData.indices);
      }

      if (gltfPrimitive.material >= 0 &&
          static_cast<size_t>(gltfPrimitive.material) < model.materials.size()) {
        const auto &gltfMaterial = model.materials[gltfPrimitive.material];
        const auto &pbr = gltfMaterial.pbrMetallicRoughness;

        primitiveData.material.normalScale = static_cast<float>(gltfMaterial.normalTexture.scale);
        primitiveData.material.baseColorFactor = glm::make_vec4(pbr.baseColorFactor.data());
        primitiveData.material.metallicFactor = pbr.metallicFactor;
        primitiveData.material.roughnessFactor = pbr.roughnessFactor;
        primitiveData.material.occlusionStrength = gltfMaterial.occlusionTexture.strength;
        primitiveData.material.emissiveFactor = glm::make_vec3(gltfMaterial.emissiveFactor.data());

        auto get_image_index = [&](int texture_index) {
          if (texture_index >= 0 && static_cast<size_t>(texture_index) < model.textures.size()) {
            return model.textures[texture_index].source;
          }
          return -1;
        };
        // Texture index assignments...
        primitiveData.baseColorTextureGltfIndex = get_image_index(pbr.baseColorTexture.index);
        primitiveData.metallicRoughnessTextureGltfIndex =
            get_image_index(pbr.metallicRoughnessTexture.index);
        primitiveData.normalTextureGltfIndex = get_image_index(gltfMaterial.normalTexture.index);
        primitiveData.occlusionTextureGltfIndex =
            get_image_index(gltfMaterial.occlusionTexture.index);
        primitiveData.emissiveTextureGltfIndex =
            get_image_index(gltfMaterial.emissiveTexture.index);

      } else {
        primitiveData.material = Material{};
      }
      meshData.primitives.emplace_back(std::move(primitiveData));
    }
    loadedScene.meshes.emplace_back(std::move(meshData));
  }

  // 2. Process Nodes
  loadedScene.nodes.resize(model.nodes.size());
  for (size_t i = 0; i < model.nodes.size(); ++i) {
    const auto &gltfNode = model.nodes[i];
    GltfNodeData &nodeData = loadedScene.nodes[i];
    nodeData.name = gltfNode.name.empty() ? ("Node_" + std::to_string(i)) : gltfNode.name;
    nodeData.meshIndex = gltfNode.mesh;

    if (!gltfNode.matrix.empty()) {
      nodeData.transform = glm::make_mat4(gltfNode.matrix.data());
    } else {
      auto translationMatrix = glm::mat4(1.0);
      auto rotationMatrix = glm::mat4(1.0);
      auto scaleMatrix = glm::mat4(1.0);
      if (!gltfNode.translation.empty()) {
        translationMatrix = glm::translate(translationMatrix, glm::vec3(gltfNode.translation[0], gltfNode.translation[1],
                                        gltfNode.translation[2]));
      }
      if (!gltfNode.rotation.empty()) {
        rotationMatrix = glm::mat4_cast(glm::quat(static_cast<float>(gltfNode.rotation[3]), static_cast<float>(gltfNode.rotation[0]),
                                     static_cast<float>(gltfNode.rotation[1]), static_cast<float>(gltfNode.rotation[2])));
      }
      if (!gltfNode.scale.empty()) {
        scaleMatrix = glm::scale(scaleMatrix, glm::vec3(gltfNode.scale[0], gltfNode.scale[1], gltfNode.scale[2]));
      }
      nodeData.transform = translationMatrix * rotationMatrix * scaleMatrix;
    }

    for (int childIdx : gltfNode.children) {
      nodeData.childrenIndices.push_back(childIdx);
      if (childIdx >= 0 && static_cast<size_t>(childIdx) < loadedScene.nodes.size()) {
        loadedScene.nodes[childIdx].parentIndex = static_cast<int>(i);
      }
    }
  }

  // 3. Identify Root Nodes
  if (model.defaultScene >= 0 && static_cast<size_t>(model.defaultScene) < model.scenes.size()) {
    for (int rootNodeIdx : model.scenes[model.defaultScene].nodes) {
      loadedScene.rootNodeIndices.push_back(rootNodeIdx);
    }
  } else if (!model.nodes.empty()) {
    for (size_t i = 0; i < loadedScene.nodes.size(); ++i) {
      if (loadedScene.nodes[i].parentIndex == -1) {
        loadedScene.rootNodeIndices.push_back(static_cast<int>(i));
      }
    }
  }
  return loadedScene;
}
