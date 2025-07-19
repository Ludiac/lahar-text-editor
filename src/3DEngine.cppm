module;

#define GLM_ENABLE_EXPERIMENTAL
#include "imgui.h"
#include "macros.hpp"
#include "primitive_types.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

export module vulkan_app:ThreeDEngine;

import vulkan_hpp;
import std;
import BS.thread_pool;

import :VulkanDevice;
import :VulkanPipeline;
import :scene;
import :mesh;
import :texture;
import :TextureStore;
import :ModelLoader;
import :SceneBuilder;
import :utils;
import :Types;
import :VMA;

namespace { // Anonymous namespace for internal helpers
std::vector<Vertex> createAxisLineVertices(const glm::vec3 &start, const glm::vec3 &end,
                                           const glm::vec3 &normalPlaceholder) {
  return {
      {.pos = start,
       .normal = normalPlaceholder,
       .uv = {0.0, 0.0},
       .tangent = {1.0, 0.0, 0.0, 1.0}},
      {.pos = end, .normal = normalPlaceholder, .uv = {1.0, 0.0}, .tangent = {1.0, 0.0, 0.0, 1.0}}};
}
std::vector<u32> createAxisLineIndices() { return {0, 1}; }
} // namespace

export class ThreeDEngine {
private:
  VulkanDevice &m_device;
  u32 m_frameCount;
  BS::thread_pool<> &m_thread_pool; // Reference to the shared thread pool

  // Rendering Resources
  std::vector<VulkanPipeline> m_graphicsPipelines;
  vk::raii::DescriptorSetLayout m_combinedMeshLayout{nullptr};
  vk::raii::DescriptorSetLayout m_sceneLayout{nullptr};
  std::vector<vk::raii::DescriptorSet> m_sceneDescriptorSets;

  // UBOs
  std::vector<VmaBuffer> m_sceneLightsUbos;
  SceneLightsUBO m_sceneLightsCpuBuffer{};
  std::vector<VmaBuffer> m_shaderTogglesUbos;
  ShaderTogglesUBO m_shaderTogglesCpuBuffer;

  // Scene and Assets
  Scene m_scene;
  std::unique_ptr<TextureStore> m_textureStore;
  std::vector<std::unique_ptr<Mesh>> m_appOwnedMeshes;
  Camera m_camera;

  // Model Loading
  std::vector<LoadedGltfScene> m_loadedGltfData;
  std::mutex m_loadedGltfDataMutex;

public:
  ThreeDEngine(VulkanDevice &device, u32 frameCount, BS::thread_pool<> &threadPool)
      : m_device(device), m_frameCount(frameCount), m_thread_pool(threadPool), m_scene(frameCount) {
    m_textureStore = std::make_unique<TextureStore>(device, device.queue);
  }
  // --- Public API ---

  std::expected<void, std::string> initialize(const vk::raii::RenderPass &renderPass,
                                              const vk::raii::PipelineCache &pipelineCache) {
    EXPECTED_VOID(m_textureStore->createDefaultTextures());
    EXPECTED_VOID(createDescriptorSetLayouts());
    EXPECTED_VOID(createGraphicsPipelines(renderPass, pipelineCache));
    EXPECTED_VOID(setupSceneLights());
    EXPECTED_VOID(setupShaderToggles());
    allocateSceneDescriptorSets();
    return {};
  }

  void loadInitialAssets() {
    createDebugAxesScene();
    // Example of submitting a model load task
    // thread_pool_.submit_task([this] {
    //   loadGltfModel("../assets/models/sarc.glb", "");
    // });
  }

  void update(u32 frameIndex, float deltaTime, vk::Extent2D swapchainExtent) {
    m_camera.updateVectors();
    m_scene.updateHierarchy(
        frameIndex, m_camera.getViewMatrix(),
        m_camera.getProjectionMatrix(static_cast<float>(swapchainExtent.width) /
                                     static_cast<float>(swapchainExtent.height)),
        deltaTime);
    m_scene.updateAllDescriptorSetContents(frameIndex);

    // Update UBOs
    std::memcpy(m_sceneLightsUbos[frameIndex].getMappedData(), &m_sceneLightsCpuBuffer,
                sizeof(SceneLightsUBO));
    std::memcpy(m_shaderTogglesUbos[frameIndex].getMappedData(), &m_shaderTogglesCpuBuffer,
                sizeof(ShaderTogglesUBO));

    // Update descriptor sets for scene-wide data
    vk::DescriptorBufferInfo lightUboInfo{.buffer = m_sceneLightsUbos[frameIndex].get(),
                                          .offset = 0,
                                          .range = sizeof(SceneLightsUBO)};
    vk::DescriptorBufferInfo togglesUboInfo{.buffer = m_shaderTogglesUbos[frameIndex].get(),
                                            .offset = 0,
                                            .range = sizeof(ShaderTogglesUBO)};
    std::array<vk::WriteDescriptorSet, 2> writeInfos = {
        vk::WriteDescriptorSet{.dstSet = *m_sceneDescriptorSets[frameIndex],
                               .dstBinding = 0,
                               .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eUniformBuffer,
                               .pBufferInfo = &lightUboInfo},
        vk::WriteDescriptorSet{.dstSet = *m_sceneDescriptorSets[frameIndex],
                               .dstBinding = 1,
                               .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eUniformBuffer,
                               .pBufferInfo = &togglesUboInfo}};
    m_device.logical().updateDescriptorSets(writeInfos, nullptr);
  }

  void draw(vk::raii::CommandBuffer &cmd, u32 frameIndex) {
    if (m_graphicsPipelines.empty() || !*m_graphicsPipelines[0].pipelineLayout) {
      return;
    }

    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_graphicsPipelines[0].pipelineLayout,
                           1, // firstSet = 1
                           {*m_sceneDescriptorSets[frameIndex]}, {});

    m_scene.draw(cmd, frameIndex);
  }

  void processGltfLoads() {
    auto newScenes = stealLoadedScenes();
    for (auto &sceneData : newScenes) {
      instanceGltfModel(sceneData);
    }
  }

  void onSwapchainRecreated(u32 newFrameCount) {
    m_frameCount = newFrameCount;
    m_scene.setImageCount(newFrameCount, m_device.descriptorPool, m_combinedMeshLayout);
    // Re-create frame-dependent resources
    setupSceneLights();
    setupShaderToggles();
    allocateSceneDescriptorSets();
  }

  // --- Getters for UI ---
  Camera &getCamera() { return m_camera; }
  Scene &getScene() { return m_scene; }
  ShaderTogglesUBO &getShaderToggles() { return m_shaderTogglesCpuBuffer; }
  SceneLightsUBO &getLightUbo() { return m_sceneLightsCpuBuffer; }

private:
  // --- Private Methods ---

  std::expected<void, std::string> createDescriptorSetLayouts() {
    std::vector<vk::DescriptorSetLayoutBinding> meshDataBindings = {
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eUniformBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eUniformBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eFragment},
        {.binding = 2,
         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eFragment},
        {.binding = 3,
         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eFragment},
        {.binding = 4,
         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eFragment},
        {.binding = 5,
         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eFragment},
        {.binding = 6,
         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eFragment},
    };
    vk::DescriptorSetLayoutCreateInfo layoutInfo{.bindingCount =
                                                     static_cast<u32>(meshDataBindings.size()),
                                                 .pBindings = meshDataBindings.data()};
    auto layoutResult = m_device.logical().createDescriptorSetLayout(layoutInfo);
    if (!layoutResult) {
      return std::unexpected("Failed to create combined mesh descriptor set layout: " +
                             vk::to_string(layoutResult.error()));
    }
    m_combinedMeshLayout = std::move(layoutResult.value());

    std::vector<vk::DescriptorSetLayoutBinding> sceneDataBindings = {
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eUniformBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eFragment},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eUniformBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment},
    };
    vk::DescriptorSetLayoutCreateInfo sceneLayoutInfo{
        .bindingCount = static_cast<u32>(sceneDataBindings.size()),
        .pBindings = sceneDataBindings.data()};
    auto sceneLayoutResult = m_device.logical().createDescriptorSetLayout(sceneLayoutInfo);
    if (!sceneLayoutResult) {
      return std::unexpected("Failed to create scene descriptor set layout: " +
                             vk::to_string(sceneLayoutResult.error()));
    }
    m_sceneLayout = std::move(sceneLayoutResult.value());

    return {};
  }

  void allocateSceneDescriptorSets() {
    std::vector<vk::DescriptorSetLayout> layouts(m_frameCount, *m_sceneLayout);
    vk::DescriptorSetAllocateInfo allocInfo{.descriptorPool = *m_device.descriptorPool,
                                            .descriptorSetCount = m_frameCount,
                                            .pSetLayouts = layouts.data()};
    m_sceneDescriptorSets = m_device.logical().allocateDescriptorSets(allocInfo).value();
  }

  std::expected<void, std::string>
  createGraphicsPipelines(const vk::raii::RenderPass &renderPass,
                          const vk::raii::PipelineCache &pipelineCache) {
    if (m_graphicsPipelines.empty()) {
      m_graphicsPipelines.resize(2);
    }

    vk::VertexInputBindingDescription bindingDescription{
        .binding = 0, .stride = sizeof(Vertex), .inputRate = vk::VertexInputRate::eVertex};
    std::array<vk::VertexInputAttributeDescription, 4> attributes = {{
        {.location = 0,
         .binding = 0,
         .format = vk::Format::eR32G32B32Sfloat,
         .offset = offsetof(Vertex, pos)},
        {.location = 1,
         .binding = 0,
         .format = vk::Format::eR32G32B32Sfloat,
         .offset = offsetof(Vertex, normal)},
        {.location = 2,
         .binding = 0,
         .format = vk::Format::eR32G32Sfloat,
         .offset = offsetof(Vertex, uv)},
        {.location = 3,
         .binding = 0,
         .format = vk::Format::eR32G32B32A32Sfloat,
         .offset = offsetof(Vertex, tangent)},
    }};
    vk::PipelineVertexInputStateCreateInfo vertexInputInfo{
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bindingDescription,
        .vertexAttributeDescriptionCount = static_cast<u32>(attributes.size()),
        .pVertexAttributeDescriptions = attributes.data()};
    vk::PipelineDepthStencilStateCreateInfo depthStencilState{.depthTestEnable = vk::True,
                                                              .depthWriteEnable = vk::True,
                                                              .depthCompareOp =
                                                                  vk::CompareOp::eLess};
    vk::PipelineColorBlendAttachmentState colorBlendAttachment{
        .blendEnable = vk::False,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};

    VulkanPipeline &mainPipeline = m_graphicsPipelines[0];
    std::vector<vk::DescriptorSetLayout> layouts = {*m_combinedMeshLayout, *m_sceneLayout};
    EXPECTED_VOID(mainPipeline.createPipelineLayout(m_device.logical(), layouts, {}));

    auto vertShaderModule = createShaderModuleFromFile(m_device.logical(), "shaders/vert.spv");
    auto fragShaderModule = createShaderModuleFromFile(m_device.logical(), "shaders/frag.spv");
    if (!vertShaderModule || !fragShaderModule) {
      return std::unexpected("Failed to load 3D shaders.");
    }

    std::vector<vk::PipelineShaderStageCreateInfo> shaderStages = {
        {.stage = vk::ShaderStageFlagBits::eVertex,
         .module = *vertShaderModule.value(),
         .pName = "main"},
        {.stage = vk::ShaderStageFlagBits::eFragment,
         .module = *fragShaderModule.value(),
         .pName = "main"}};

    vk::PipelineInputAssemblyStateCreateInfo inputAssembly{
        .topology = vk::PrimitiveTopology::eTriangleList};
    EXPECTED_VOID(mainPipeline.createGraphicsPipeline(
        m_device.logical(), pipelineCache, shaderStages, vertexInputInfo, inputAssembly, renderPass,
        &colorBlendAttachment, &depthStencilState));

    VulkanPipeline &linePipeline = m_graphicsPipelines[1];
    EXPECTED_VOID(linePipeline.createPipelineLayout(m_device.logical(), layouts, {}));
    vk::PipelineInputAssemblyStateCreateInfo lineInputAssembly{
        .topology = vk::PrimitiveTopology::eLineList};
    EXPECTED_VOID(linePipeline.createGraphicsPipeline(
        m_device.logical(), pipelineCache, shaderStages, vertexInputInfo, lineInputAssembly,
        renderPass, &colorBlendAttachment, &depthStencilState));

    return {};
  }

  std::expected<void, std::string> setupShaderToggles() {
    vk::DeviceSize bufferSize = sizeof(ShaderTogglesUBO);
    m_shaderTogglesUbos.resize(m_frameCount);
    for (size_t i = 0; i < m_frameCount; i++) {
      vk::BufferCreateInfo bufferInfo{.size = bufferSize,
                                      .usage = vk::BufferUsageFlagBits::eUniformBuffer};
      vma::AllocationCreateInfo allocInfo{
          .flags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite |
                   vma::AllocationCreateFlagBits::eMapped,
          .usage = vma::MemoryUsage::eAutoPreferHost};
      auto bufferResult = m_device.createBufferVMA(bufferInfo, allocInfo);
      if (!bufferResult) {
        return std::unexpected("Failed to create shader toggles UBO for frame " +
                               std::to_string(i));
      }
      m_shaderTogglesUbos[i] = std::move(bufferResult.value());
    }
    return {};
  }

  std::expected<void, std::string> setupSceneLights() {
    m_sceneLightsCpuBuffer.lightCount = 2;
    m_sceneLightsCpuBuffer.lights[0].position = {-20.0, -20.0, -20.0, 1.0};
    m_sceneLightsCpuBuffer.lights[0].color = {150.0, 150.0, 150.0, 1.0};
    m_sceneLightsCpuBuffer.lights[1].position = {20.0, -30.0, -15.0, 1.0};
    m_sceneLightsCpuBuffer.lights[1].color = {200.0, 150.0, 50.0, 1.0};

    vk::DeviceSize bufferSize = sizeof(SceneLightsUBO);
    m_sceneLightsUbos.resize(m_frameCount);
    for (size_t i = 0; i < m_frameCount; i++) {
      vk::BufferCreateInfo bufferInfo{.size = bufferSize,
                                      .usage = vk::BufferUsageFlagBits::eUniformBuffer};
      vma::AllocationCreateInfo allocInfo{
          .flags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite |
                   vma::AllocationCreateFlagBits::eMapped,
          .usage = vma::MemoryUsage::eAutoPreferHost};
      auto bufferResult = m_device.createBufferVMA(bufferInfo, allocInfo);
      if (!bufferResult) {
        return std::unexpected("Failed to create light UBO for frame " + std::to_string(i));
      }
      m_sceneLightsUbos[i] = std::move(bufferResult.value());
    }
    return {};
  }

  void createDebugAxesScene() {
    if (m_graphicsPipelines.size() < 2 || !*m_graphicsPipelines[1].pipeline) {
      return;
    }

    VulkanPipeline *linePipeline = &m_graphicsPipelines[1];
    float axisLength = 10000.0;

    auto createAxis = [&](const std::string &name, glm::vec3 start, glm::vec3 end,
                          std::array<u8, 4> color, glm::vec3 tangent) {
      Material axisMaterial;
      PBRTextures axisTextures = m_textureStore->getAllDefaultTextures();
      axisTextures.baseColor = m_textureStore->getColorTexture(color);
      auto axisMesh =
          std::make_unique<Mesh>(m_device, name, createAxisLineVertices(start, end, tangent),
                                 createAxisLineIndices(), axisMaterial, axisTextures, m_frameCount);
      m_appOwnedMeshes.emplace_back(std::move(axisMesh));
      m_scene.createNode(
          {.mesh = m_appOwnedMeshes.back().get(), .pipeline = linePipeline, .name = name + "_Node"},
          m_device.descriptorPool, m_combinedMeshLayout);
    };

    createAxis("X_Axis", {-axisLength, 0, 0}, {axisLength, 0, 0}, {255, 0, 0, 255}, {0, 1, 0});
    createAxis("Y_Axis", {0, -axisLength, 0}, {0, axisLength, 0}, {0, 255, 0, 255}, {1, 0, 0});
    createAxis("Z_Axis", {0, 0, -axisLength}, {0, 0, axisLength}, {0, 0, 255, 255}, {1, 0, 0});
  }

  void loadGltfModel(const std::string &filePath, const std::string &baseDir) {
    auto loadedGltfDataResult = loadGltfFile(filePath, baseDir);
    if (!loadedGltfDataResult) {
      std::println("Failed to load GLTF file '{}': {}", filePath, loadedGltfDataResult.error());
      return;
    }
    std::lock_guard lock{m_loadedGltfDataMutex};
    m_loadedGltfData.emplace_back(*loadedGltfDataResult);
  }

  void instanceGltfModel(const LoadedGltfScene &gltfData) {
    auto builtMeshesResult =
        populateSceneFromGltf(m_scene, gltfData, m_device, *m_textureStore,
                              m_graphicsPipelines.data(), m_frameCount, m_combinedMeshLayout);
    if (!builtMeshesResult) {
      std::println("Failed to build engine scene from GLTF data: {}", builtMeshesResult.error());
      return;
    }
    for (auto &meshPtr : builtMeshesResult->engineMeshes) {
      m_appOwnedMeshes.emplace_back(std::move(meshPtr));
    }
  }

  std::vector<LoadedGltfScene> stealLoadedScenes() {
    std::vector<LoadedGltfScene> out;
    {
      std::lock_guard lock{m_loadedGltfDataMutex};
      out = std::move(m_loadedGltfData);
      m_loadedGltfData.clear();
    }
    return out;
  }
};
