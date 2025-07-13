module;

#include "macros.hpp"
#include "primitive_types.hpp"
#include <glm/glm.hpp>

export module vulkan_app:SceneBuilder;

import std;
import :VulkanDevice;
import :mesh;
import :scene;
import :texture;
import :TextureStore;
import :ModelLoader; // For LoadedGltfScene, GltfImageData, etc.
import :VulkanPipeline;

export struct BuiltSceneMeshes {
  std::vector<std::unique_ptr<Mesh>> engineMeshes;
  u32 imageCount{0}; // important for assuring later that this number is equal to app current number
};

export [[nodiscard]] std::expected<BuiltSceneMeshes, std::string>
populateSceneFromGltf(Scene &targetScene, const LoadedGltfScene &gltfData, VulkanDevice &device,
                      TextureStore &textureStore, VulkanPipeline *defaultPipeline, u32 imageCount,
                      const vk::raii::DescriptorSetLayout &descriptorLayout) {
  if (!defaultPipeline)
    return std::unexpected("SceneBuilder: Default pipeline is null.");

  BuiltSceneMeshes builtMeshesResult;
  std::map<int, std::vector<Mesh *>> gltfMeshIndexToEngineMeshesMap;

  // 1. Create Engine Meshes
  for (int gltfMeshIdx = 0; gltfMeshIdx < gltfData.meshes.size(); ++gltfMeshIdx) {
    const auto &gltfMesh = gltfData.meshes[gltfMeshIdx];
    std::vector<Mesh *> engineMeshesForThisGltfMesh;

    for (int primIdx = 0; primIdx < gltfMesh.primitives.size(); ++primIdx) {
      const auto &gltfPrimitive = gltfMesh.primitives[primIdx];
      if (gltfPrimitive.vertices.empty() || gltfPrimitive.indices.empty())
        continue;

      Material engineMaterial = gltfPrimitive.material;
      PBRTextures enginePbrTextures;

      // Helper to load a texture from GLTF data or return a default
      auto load_gltf_texture =
          [&](int image_index, const std::string &texture_type_name,
              std::shared_ptr<Texture> fallback_texture) -> std::shared_ptr<Texture> {
        if (image_index >= 0) {
          auto imageIt = gltfData.images.find(image_index);
          if (imageIt != gltfData.images.end()) {
            GltfImageData loadedImgData = imageIt->second;
            // Ensure SRGB status is set correctly for color data
            return textureStore.getTextureFromData(loadedImgData);
          } else {
            std::println("Warning: GLTF material specified {} texture (image index {}), but data "
                         "not found. Using default.",
                         texture_type_name, image_index);
          }
        }
        // std::println("using default texture {}", texture_type_name);
        return fallback_texture;
      };

      enginePbrTextures.baseColor = load_gltf_texture(
          gltfPrimitive.baseColorTextureGltfIndex, "BaseColor", textureStore.getDefaultTexture());
      enginePbrTextures.normal = load_gltf_texture(gltfPrimitive.normalTextureGltfIndex, "Normal",
                                                   textureStore.getDefaultNormalTexture());
      enginePbrTextures.metallicRoughness =
          load_gltf_texture(gltfPrimitive.metallicRoughnessTextureGltfIndex, "MetallicRoughness",
                            textureStore.getDefaultMRTexture());
      enginePbrTextures.occlusion = load_gltf_texture(
          gltfPrimitive.occlusionTextureGltfIndex, "Occlusion", textureStore.getDefaultTexture());
      enginePbrTextures.emissive =
          load_gltf_texture(gltfPrimitive.emissiveTextureGltfIndex, "Emissive",
                            textureStore.getDefaultEmissiveTexture());

      std::string engineMeshName = gltfMesh.name + "_Prim" + std::to_string(primIdx);
      auto newEngineMesh = std::make_unique<Mesh>(
          device, engineMeshName, std::vector<Vertex>(gltfPrimitive.vertices),
          std::vector<u32>(gltfPrimitive.indices), engineMaterial, enginePbrTextures, imageCount);

      engineMeshesForThisGltfMesh.push_back(newEngineMesh.get());
      builtMeshesResult.engineMeshes.emplace_back(std::move(newEngineMesh));
    }
    gltfMeshIndexToEngineMeshesMap[gltfMeshIdx] = engineMeshesForThisGltfMesh;
  }

  // 2. Create SceneNodes (First Pass - create all nodes, initially children of fatherNode_)
  // All nodes are initially created with a nullptr parent. Scene::createNode will assign them
  // as children of fatherNode_ if no explicit parent is given.
  std::vector<SceneNode *> createdEngineNodes(gltfData.nodes.size(), nullptr);
  for (size_t gltfNodeIdx = 0; gltfNodeIdx < gltfData.nodes.size(); ++gltfNodeIdx) {
    const auto &gltfNode = gltfData.nodes[gltfNodeIdx];
    SceneNodeCreateInfo nodeCreateInfo{
        .transform = decomposeFromMatrix(gltfNode.transform),
        .pipeline = defaultPipeline,
        // Set parent to nullptr initially. Scene::createNode will make it a child of fatherNode_
        // if no parent is explicitly set. Re-parenting will happen in the second pass.
        .parent = nullptr,
        .name = gltfNode.name.empty() ? ("GLTFNode_" + std::to_string(gltfNodeIdx)) : gltfNode.name,
    };
    SceneNode *engineNodeForGltfNode = nullptr;
    if (gltfNode.meshIndex >= 0) {
      const auto &it = gltfMeshIndexToEngineMeshesMap.find(gltfNode.meshIndex);
      if (it != gltfMeshIndexToEngineMeshesMap.end()) {
        const auto &primitiveEngineMeshes = it->second;
        if (primitiveEngineMeshes.empty()) {
          engineNodeForGltfNode = targetScene.createNode(nodeCreateInfo);
        } else if (primitiveEngineMeshes.size() == 1) {
          nodeCreateInfo.mesh = primitiveEngineMeshes[0];
          engineNodeForGltfNode =
              targetScene.createNode(nodeCreateInfo, device.descriptorPool_, descriptorLayout);
        } else {
          // If a glTF node has multiple primitives, create a parent node for them
          // and then create child nodes for each primitive.
          // The parent is set to nullptr here; it will be re-parented in the next loop.
          engineNodeForGltfNode = targetScene.createNode(nodeCreateInfo);
          for (Mesh *primitiveMesh : primitiveEngineMeshes) {
            SceneNodeCreateInfo childNodeCi{
                .mesh = primitiveMesh,
                .transform = Transform{}, // Identity transform relative to its parent
                .pipeline = defaultPipeline,
                .parent = engineNodeForGltfNode, // This child *does* have a parent right away
                .name = primitiveMesh->getName(),
            };
            targetScene.createNode(childNodeCi, device.descriptorPool_, descriptorLayout);
          }
        }
      } else {
        engineNodeForGltfNode = targetScene.createNode(nodeCreateInfo);
      }
    } else {
      engineNodeForGltfNode = targetScene.createNode(nodeCreateInfo);
    }
    createdEngineNodes[gltfNodeIdx] = engineNodeForGltfNode;
  }

  // 3. Build Hierarchy (Second Pass - establish parent-child relationships)
  // Leverage the improved SceneNode::addChild to automatically handle re-parenting.
  for (size_t gltfNodeIdx = 0; gltfNodeIdx < gltfData.nodes.size(); ++gltfNodeIdx) {
    const auto &gltfNode = gltfData.nodes[gltfNodeIdx];
    SceneNode *engineParentNode = createdEngineNodes[gltfNodeIdx];
    if (!engineParentNode)
      continue;

    for (int childGltfNodeIndex : gltfNode.childrenIndices) {
      if (childGltfNodeIndex >= 0 && childGltfNodeIndex < createdEngineNodes.size()) {
        SceneNode *engineChildNode = createdEngineNodes[childGltfNodeIndex];
        if (engineChildNode) {
          // The improved SceneNode::addChild will handle removing the child from its old parent
          // (which would be fatherNode_ if it was initially unparented) and adding it
          // to the new parent (engineParentNode) correctly.
          engineParentNode->addChild(engineChildNode);
        }
      }
    }
  }

  builtMeshesResult.imageCount = imageCount;
  return builtMeshesResult;
}
