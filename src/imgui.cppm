module;

#include "imgui.h"
#include "macros.hpp"
#include "primitive_types.hpp"
#include <glm/glm.hpp>

export module vulkan_app:imgui;

import vulkan_hpp;
import std;
import :VulkanWindow;
import :VulkanDevice;
import :VulkanInstance;
import :scene;
import :TextSystem;

// Forward-declare the Camera struct, assuming it's in its own module/header.
struct Camera;

// --- Forward declarations for internal (static) rendering functions ---
// NOTE: p_open parameter is removed as they no longer manage their own window
static void RenderShaderTogglesContent(ShaderTogglesUBO &toggles);
static void RenderTextContent(i32 &fontSizeMultiplier, TextToggles &textToggles);
static void RenderCameraControlContent(Camera &camera);
static void RenderVulkanStateContent(VulkanDevice &device, VulkanWindow &wd, float frameTime);
static void RenderSceneHierarchyMaterialEditorContent(Scene &scene, u32 currentFrameIndex);
static void RenderLightControlContent(SceneLightsUBO &lightUBO);
static void RenderSceneNodeRecursive(SceneNode *node, u32 currentFrameIndex);
static bool EditMaterialProperties(const std::string &materialOwnerName, Material &material);

export struct ImGuiMenu {
  // Only one flag now to control the visibility of the single debug window
  bool showDebugSettingsWindow = true;

  void renderMegaMenu(ImGuiMenu &imguiMenu, VulkanWindow &wd, VulkanDevice &device, Camera &camera,
                      Scene &scene, TextSystem &textSystem, ShaderTogglesUBO &shaderToggles,
                      SceneLightsUBO &lightUBO, TextToggles &textToggles, i32 fontSizeMultiplier,
                      u32 currentFrameIndex, float frameTime) {

    // Render the single, combined debug settings window if its flag is true.
    if (imguiMenu.showDebugSettingsWindow) {
      // Begin the single debug window. It is movable by default.
      if (ImGui::Begin("Debug Settings", &imguiMenu.showDebugSettingsWindow,
                       ImGuiWindowFlags_AlwaysAutoResize)) {
        // Shader Toggles section
        if (ImGui::CollapsingHeader("Shader Toggles", ImGuiTreeNodeFlags_DefaultOpen)) {
          RenderShaderTogglesContent(shaderToggles);
        }
        ImGui::Separator(); // Add a separator for visual distinction

        // Text Parameters section
        if (ImGui::CollapsingHeader("Text Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
          RenderTextContent(fontSizeMultiplier, textToggles);
        }
        ImGui::Separator();

        // Camera Controls section
        if (ImGui::CollapsingHeader("Camera Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
          RenderCameraControlContent(camera);
        }
        ImGui::Separator();

        // Vulkan State section
        if (ImGui::CollapsingHeader("Vulkan State", ImGuiTreeNodeFlags_DefaultOpen)) {
          RenderVulkanStateContent(device, wd, frameTime);
        }
        ImGui::Separator();

        // Scene Inspector section
        if (ImGui::CollapsingHeader("Scene Inspector", ImGuiTreeNodeFlags_DefaultOpen)) {
          RenderSceneHierarchyMaterialEditorContent(scene, currentFrameIndex);
        }
        ImGui::Separator();

        // Light Controls section
        if (ImGui::CollapsingHeader("Light Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
          RenderLightControlContent(lightUBO);
        }
      }
      ImGui::End(); // End of the single "Debug Settings" window
    }
  }
};

// --- Internal (static) Implementations of Debug Windows Content ---

/**
 * @brief Renders controls for toggling various shader features. (Content only)
 */
static void RenderShaderTogglesContent(ShaderTogglesUBO &toggles) {
  ImGui::Checkbox("Normal Mapping", (bool *)&toggles.useNormalMapping);
  ImGui::Checkbox("Occlusion", (bool *)&toggles.useOcclusion);
  ImGui::Checkbox("Emission", (bool *)&toggles.useEmission);
  ImGui::Checkbox("Lights", (bool *)&toggles.useLights);
  ImGui::Checkbox("Ambient", (bool *)&toggles.useAmbient);
}

/**
 * @brief Renders controls for text rendering parameters. (Content only)
 */
static void RenderTextContent(i32 &fontSizeMultiplier, TextToggles &textToggles) {
  const static i32 min_stages = 1;
  const static i32 max_stages = 16;
  ImGui::SliderInt("fontSizeMultiplier", &fontSizeMultiplier, -10, 50);
  ImGui::SliderFloat("SDF Weight", &textToggles.sdf_weight, 0.0, 1.0);
  ImGui::SliderFloat("PX Range Additive", &textToggles.pxRangeDirectAdditive, -10.0, 10.0);

  ImGui::Text("Anti Aliasing Mode");
  ImGui::RadioButton("None", &textToggles.antiAliasingMode, 0);
  ImGui::SameLine();
  ImGui::RadioButton("Smoothstep", &textToggles.antiAliasingMode, 1);
  ImGui::SameLine();
  ImGui::RadioButton("Linear Clamp", &textToggles.antiAliasingMode, 2);
  ImGui::SameLine();
  ImGui::RadioButton("Staged", &textToggles.antiAliasingMode, 3);

  if (textToggles.antiAliasingMode == 2) { // Linear Clamp
    ImGui::SliderFloat("Inner AA Depth", &textToggles.start_fade_px, -2.0, 2.0);
    ImGui::SliderFloat("Outer AA Depth", &textToggles.end_fade_px, -2.0, 2.0);
  }

  if (textToggles.antiAliasingMode == 3) { // Staged
    ImGui::Separator();
    ImGui::Text("Staged AA Parameters");
    ImGui::SliderScalar("Num Stages", ImGuiDataType_U32, &textToggles.num_stages, &min_stages,
                        &max_stages, "%u");
    ImGui::Text("Rounding Direction");
    ImGui::RadioButton("Down", &textToggles.rounding_direction, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Up", &textToggles.rounding_direction, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Nearest", &textToggles.rounding_direction, 2);
  }
}

/**
 * @brief Renders controls for camera properties. (Content only)
 */
static void RenderCameraControlContent(Camera &camera) {
  ImGui::Checkbox("Mouse Control", &camera.MouseCaptured);
  ImGui::SliderFloat3("Position", &camera.Position.x, -100.0, 100.0);
  ImGui::SliderFloat("Yaw", &camera.Yaw, -180.0, 180.0);
  ImGui::SliderFloat("Pitch", &camera.Pitch, -89.0, 89.0);
  ImGui::SliderFloat("Roll", &camera.Roll, -180.0, 180.0);
  ImGui::SliderFloat("FOV", &camera.Zoom, 1.0, 120.0);
  ImGui::SliderFloat("Near Plane", &camera.Near, 0.01, 1000.0);
  ImGui::SliderFloat("Far Plane", &camera.Far, 0.01, 1000.0);
  ImGui::SliderFloat("Move Speed", &camera.MovementSpeed, 0.1, 1000.0);
  ImGui::SliderFloat("Mouse Sens", &camera.MouseSensitivity, 0.01, 1.0);
}

/**
 * @brief Renders debug information about the Vulkan state. (Content only)
 */
static void RenderVulkanStateContent(VulkanDevice &device, VulkanWindow &wd, float frameTime) {
  if (ImGui::CollapsingHeader("Physical Device", ImGuiTreeNodeFlags_DefaultOpen)) {
    vk::PhysicalDeviceProperties props = device.physical().getProperties();
    ImGui::Text("GPU: %s", props.deviceName.data());
  }

  if (ImGui::CollapsingHeader("Swapchain", ImGuiTreeNodeFlags_DefaultOpen)) {
    // ImGui::Text("Present Mode: %s", vk::to_string(wd.presentMode).c_str());
    // ImGui::Text("Swapchain Images: %zu", wd.getImageCount());
    // ImGui::Text("Extent: %dx%d", wd.swapchainExtent.width, wd.swapchainExtent.height);
    // ImGui::Text("Format: %s", vk::to_string(wd.surfaceFormat.format).c_str());
  }

  if (ImGui::CollapsingHeader("Timing", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Text("Frame Time: %.3f ms", frameTime * 1000.0);
    ImGui::Text("FPS: %.1f", frameTime > 0.0 ? (1.0 / frameTime) : 0.0);

    static std::vector<float> frameTimes;
    frameTimes.push_back(frameTime * 1000.0f);
    if (frameTimes.size() > 120) {
      frameTimes.erase(frameTimes.begin());
    }
    ImGui::PlotLines("Frame Times (ms)", frameTimes.data(), frameTimes.size(), 0, nullptr, 0.0f,
                     33.3f, ImVec2(0, 80));
  }
}

/**
 * @brief Renders the main scene hierarchy and material editor. (Content only)
 */
static void RenderSceneHierarchyMaterialEditorContent(Scene &scene, u32 currentFrameIndex) {
  // The internal tree structure for the scene graph itself
  if (ImGui::TreeNodeEx("Scene Graph", ImGuiTreeNodeFlags_DefaultOpen)) {
    RenderSceneNodeRecursive(scene.fatherNode_.get(), currentFrameIndex);
    ImGui::TreePop();
  }
}

/**
 * @brief Renders controls for scene lights. (Content only)
 */
static void RenderLightControlContent(SceneLightsUBO &lightUBO) {
  ImGui::SliderInt("Light Count", &lightUBO.lightCount, 0, 10); // MAX_LIGHTS
  ImGui::Separator();
  for (int i = 0; i < lightUBO.lightCount; ++i) {
    std::string label = "Light " + std::to_string(i);
    ImGui::PushID(i);
    if (ImGui::TreeNode(label.c_str())) {
      ImGui::DragFloat3("Position", &lightUBO.lights[i].position.x, 0.5f);
      ImGui::ColorEdit3("Color", &lightUBO.lights[i].color.x);
      ImGui::DragFloat("Intensity", &lightUBO.lights[i].color.w, 1.0f, 0.0f, 10000.0f);
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
}

/**
 * @brief Recursively renders an ImGui tree for a SceneNode and its children.
 * (No change to this function, it's an internal helper for scene display)
 */
static void RenderSceneNodeRecursive(SceneNode *node, u32 currentFrameIndex) {
  if (node == nullptr)
    return;

  ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                 ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                 ImGuiTreeNodeFlags_DefaultOpen;
  if (node->children.empty()) {
    nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
  }

  bool nodeIsOpen = ImGui::TreeNodeEx((void *)(intptr_t)node, nodeFlags, "%s", node->name.c_str());

  if (nodeIsOpen) {
    if (node->mesh != nullptr) {
      if (EditMaterialProperties(node->mesh->getName(), node->mesh->getMaterial())) {
        node->mesh->updateMaterialUniformBufferData(currentFrameIndex);
        node->mesh->updateDescriptorSetContents(currentFrameIndex);
      }
    }

    if (!(nodeFlags & ImGuiTreeNodeFlags_Leaf)) {
      for (SceneNode *child : node->children) {
        RenderSceneNodeRecursive(child, currentFrameIndex);
      }
      ImGui::TreePop();
    }
  }
}

/**
 * @brief Renders ImGui controls for a single material's properties.
 * (No change to this function, it's an internal helper for material editing)
 */
static bool EditMaterialProperties(const std::string &materialOwnerName, Material &material) {
  bool changed = false;
  if (ImGui::TreeNode("Material")) {
    changed |= ImGui::ColorEdit4("Base Color Factor", &material.baseColorFactor.x);
    changed |= ImGui::SliderFloat("Metallic Factor", &material.metallicFactor, 0.0, 1.0);
    changed |= ImGui::SliderFloat("Roughness Factor", &material.roughnessFactor, 0.0, 1.0);
    changed |= ImGui::SliderFloat("Occlusion Strength", &material.occlusionStrength, 0.0, 1.0);
    changed |= ImGui::ColorEdit3("Emissive Factor", &material.emissiveFactor.x);
    ImGui::TreePop();
  }
  return changed;
}
