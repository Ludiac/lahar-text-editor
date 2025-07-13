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
                                           const glm::vec3 &normal_placeholder) {
  return {{.pos = start,
           .normal = normal_placeholder,
           .uv = {0.0, 0.0},
           .tangent = {1.0, 0.0, 0.0, 1.0}},
          {.pos = end,
           .normal = normal_placeholder,
           .uv = {1.0, 0.0},
           .tangent = {1.0, 0.0, 0.0, 1.0}}};
}
std::vector<u32> createAxisLineIndices() { return {0, 1}; }
} // namespace

export class ThreeDEngine {
private:
  VulkanDevice &device_;
  u32 frameCount_;
  BS::thread_pool<> &thread_pool_; // Reference to the shared thread pool

  // Rendering Resources
  std::vector<VulkanPipeline> graphicsPipelines_;
  vk::raii::DescriptorSetLayout combinedMeshLayout_{nullptr};
  vk::raii::DescriptorSetLayout sceneLayout_{nullptr};
  std::vector<vk::raii::DescriptorSet> sceneDescriptorSets_;

  // UBOs
  std::vector<VmaBuffer> sceneLightsUbos_;
  SceneLightsUBO sceneLightsCpuBuffer_;
  std::vector<VmaBuffer> shaderTogglesUbos_;
  ShaderTogglesUBO shaderTogglesCpuBuffer_;

  // Scene and Assets
  Scene scene_;
  std::unique_ptr<TextureStore> textureStore_;
  std::vector<std::unique_ptr<Mesh>> appOwnedMeshes_;
  Camera camera_;

  // Model Loading
  std::vector<LoadedGltfScene> loadedGltfData_;
  std::mutex loadedGltfDataMutex_;

public:
  ThreeDEngine(VulkanDevice &device, u32 frameCount, BS::thread_pool<> &thread_pool)
      : device_(device), frameCount_(frameCount), thread_pool_(thread_pool), scene_(frameCount) {
    textureStore_ = std::make_unique<TextureStore>(device, device.queue_);
  }
  // --- Public API ---

  std::expected<void, std::string> initialize(const vk::raii::RenderPass &renderPass,
                                              const vk::raii::PipelineCache &pipelineCache) {
    EXPECTED_VOID(textureStore_->createDefaultTextures());
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
    camera_.updateVectors();
    scene_.updateHierarchy(frameIndex, camera_.GetViewMatrix(),
                           camera_.GetProjectionMatrix(static_cast<float>(swapchainExtent.width) /
                                                       static_cast<float>(swapchainExtent.height)),
                           deltaTime);
    scene_.updateAllDescriptorSetContents(frameIndex);

    // Update UBOs
    std::memcpy(sceneLightsUbos_[frameIndex].getMappedData(), &sceneLightsCpuBuffer_,
                sizeof(SceneLightsUBO));
    std::memcpy(shaderTogglesUbos_[frameIndex].getMappedData(), &shaderTogglesCpuBuffer_,
                sizeof(ShaderTogglesUBO));

    // Update descriptor sets for scene-wide data
    vk::DescriptorBufferInfo lightUboInfo{
        .buffer = sceneLightsUbos_[frameIndex].get(), .offset = 0, .range = sizeof(SceneLightsUBO)};
    vk::DescriptorBufferInfo togglesUboInfo{.buffer = shaderTogglesUbos_[frameIndex].get(),
                                            .offset = 0,
                                            .range = sizeof(ShaderTogglesUBO)};
    std::array<vk::WriteDescriptorSet, 2> writeInfos = {
        vk::WriteDescriptorSet{.dstSet = *sceneDescriptorSets_[frameIndex],
                               .dstBinding = 0,
                               .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eUniformBuffer,
                               .pBufferInfo = &lightUboInfo},
        vk::WriteDescriptorSet{.dstSet = *sceneDescriptorSets_[frameIndex],
                               .dstBinding = 1,
                               .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eUniformBuffer,
                               .pBufferInfo = &togglesUboInfo}};
    device_.logical().updateDescriptorSets(writeInfos, nullptr);
  }

  void draw(vk::raii::CommandBuffer &cmd, u32 frameIndex) {
    if (graphicsPipelines_.empty() || !*graphicsPipelines_[0].pipelineLayout) {
      return;
    }

    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *graphicsPipelines_[0].pipelineLayout,
                           1, // firstSet = 1
                           {*sceneDescriptorSets_[frameIndex]}, {});

    scene_.draw(cmd, frameIndex);
  }

  void processGltfLoads() {
    auto newScenes = stealLoadedScenes();
    for (auto &sceneData : newScenes) {
      instanceGltfModel(sceneData);
    }
  }

  void onSwapchainRecreated(u32 newFrameCount) {
    frameCount_ = newFrameCount;
    scene_.setImageCount(newFrameCount, device_.descriptorPool_, combinedMeshLayout_);
    // Re-create frame-dependent resources
    setupSceneLights();
    setupShaderToggles();
    allocateSceneDescriptorSets();
  }

  // --- Getters for UI ---
  Camera &getCamera() { return camera_; }
  Scene &getScene() { return scene_; }
  ShaderTogglesUBO &getShaderToggles() { return shaderTogglesCpuBuffer_; }
  SceneLightsUBO &getLightUbo() { return sceneLightsCpuBuffer_; }

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
    auto layoutResult = device_.logical().createDescriptorSetLayout(layoutInfo);
    if (!layoutResult) {
      return std::unexpected("Failed to create combined mesh descriptor set layout: " +
                             vk::to_string(layoutResult.error()));
    }
    combinedMeshLayout_ = std::move(layoutResult.value());

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
    auto sceneLayoutResult = device_.logical().createDescriptorSetLayout(sceneLayoutInfo);
    if (!sceneLayoutResult) {
      return std::unexpected("Failed to create scene descriptor set layout: " +
                             vk::to_string(sceneLayoutResult.error()));
    }
    sceneLayout_ = std::move(sceneLayoutResult.value());

    return {};
  }

  void allocateSceneDescriptorSets() {
    std::vector<vk::DescriptorSetLayout> layouts(frameCount_, *sceneLayout_);
    vk::DescriptorSetAllocateInfo allocInfo{.descriptorPool = *device_.descriptorPool_,
                                            .descriptorSetCount = frameCount_,
                                            .pSetLayouts = layouts.data()};
    sceneDescriptorSets_ = device_.logical().allocateDescriptorSets(allocInfo).value();
  }

  std::expected<void, std::string>
  createGraphicsPipelines(const vk::raii::RenderPass &renderPass,
                          const vk::raii::PipelineCache &pipelineCache) {
    if (graphicsPipelines_.empty()) {
      graphicsPipelines_.resize(2);
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

    VulkanPipeline &mainPipeline = graphicsPipelines_[0];
    std::vector<vk::DescriptorSetLayout> layouts = {*combinedMeshLayout_, *sceneLayout_};
    EXPECTED_VOID(mainPipeline.createPipelineLayout(device_.logical(), layouts, {}));

    auto vertShaderModule = createShaderModuleFromFile(device_.logical(), "shaders/vert.spv");
    auto fragShaderModule = createShaderModuleFromFile(device_.logical(), "shaders/frag.spv");
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
        device_.logical(), pipelineCache, shaderStages, vertexInputInfo, inputAssembly, renderPass,
        &colorBlendAttachment, &depthStencilState));

    VulkanPipeline &linePipeline = graphicsPipelines_[1];
    EXPECTED_VOID(linePipeline.createPipelineLayout(device_.logical(), layouts, {}));
    vk::PipelineInputAssemblyStateCreateInfo lineInputAssembly{
        .topology = vk::PrimitiveTopology::eLineList};
    EXPECTED_VOID(linePipeline.createGraphicsPipeline(
        device_.logical(), pipelineCache, shaderStages, vertexInputInfo, lineInputAssembly,
        renderPass, &colorBlendAttachment, &depthStencilState));

    return {};
  }

  std::expected<void, std::string> setupShaderToggles() {
    vk::DeviceSize bufferSize = sizeof(ShaderTogglesUBO);
    shaderTogglesUbos_.resize(frameCount_);
    for (size_t i = 0; i < frameCount_; i++) {
      vk::BufferCreateInfo bufferInfo{.size = bufferSize,
                                      .usage = vk::BufferUsageFlagBits::eUniformBuffer};
      vma::AllocationCreateInfo allocInfo{
          .flags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite |
                   vma::AllocationCreateFlagBits::eMapped,
          .usage = vma::MemoryUsage::eAutoPreferHost};
      auto bufferResult = device_.createBufferVMA(bufferInfo, allocInfo);
      if (!bufferResult) {
        return std::unexpected("Failed to create shader toggles UBO for frame " +
                               std::to_string(i));
      }
      shaderTogglesUbos_[i] = std::move(bufferResult.value());
    }
    return {};
  }

  std::expected<void, std::string> setupSceneLights() {
    sceneLightsCpuBuffer_.lightCount = 2;
    sceneLightsCpuBuffer_.lights[0].position = {-20.0, -20.0, -20.0, 1.0};
    sceneLightsCpuBuffer_.lights[0].color = {150.0, 150.0, 150.0, 1.0};
    sceneLightsCpuBuffer_.lights[1].position = {20.0, -30.0, -15.0, 1.0};
    sceneLightsCpuBuffer_.lights[1].color = {200.0, 150.0, 50.0, 1.0};

    vk::DeviceSize bufferSize = sizeof(SceneLightsUBO);
    sceneLightsUbos_.resize(frameCount_);
    for (size_t i = 0; i < frameCount_; i++) {
      vk::BufferCreateInfo bufferInfo{.size = bufferSize,
                                      .usage = vk::BufferUsageFlagBits::eUniformBuffer};
      vma::AllocationCreateInfo allocInfo{
          .flags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite |
                   vma::AllocationCreateFlagBits::eMapped,
          .usage = vma::MemoryUsage::eAutoPreferHost};
      auto bufferResult = device_.createBufferVMA(bufferInfo, allocInfo);
      if (!bufferResult) {
        return std::unexpected("Failed to create light UBO for frame " + std::to_string(i));
      }
      sceneLightsUbos_[i] = std::move(bufferResult.value());
    }
    return {};
  }

  void createDebugAxesScene() {
    if (graphicsPipelines_.size() < 2 || !*graphicsPipelines_[1].pipeline) {
      return;
    }

    VulkanPipeline *linePipeline = &graphicsPipelines_[1];
    float axisLength = 10000.0;

    auto createAxis = [&](const std::string &name, glm::vec3 start, glm::vec3 end,
                          std::array<u8, 4> color, glm::vec3 tangent) {
      Material axisMaterial;
      PBRTextures axisTextures = textureStore_->getAllDefaultTextures();
      axisTextures.baseColor = textureStore_->getColorTexture(color);
      auto axisMesh =
          std::make_unique<Mesh>(device_, name, createAxisLineVertices(start, end, tangent),
                                 createAxisLineIndices(), axisMaterial, axisTextures, frameCount_);
      appOwnedMeshes_.emplace_back(std::move(axisMesh));
      scene_.createNode(
          {.mesh = appOwnedMeshes_.back().get(), .pipeline = linePipeline, .name = name + "_Node"},
          device_.descriptorPool_, combinedMeshLayout_);
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
    std::lock_guard lock{loadedGltfDataMutex_};
    loadedGltfData_.emplace_back(*loadedGltfDataResult);
  }

  void instanceGltfModel(const LoadedGltfScene &gltfData) {
    auto builtMeshesResult =
        populateSceneFromGltf(scene_, gltfData, device_, *textureStore_, graphicsPipelines_.data(),
                              frameCount_, combinedMeshLayout_);
    if (!builtMeshesResult) {
      std::println("Failed to build engine scene from GLTF data: {}", builtMeshesResult.error());
      return;
    }
    for (auto &mesh_ptr : builtMeshesResult->engineMeshes) {
      appOwnedMeshes_.emplace_back(std::move(mesh_ptr));
    }
  }

  std::vector<LoadedGltfScene> stealLoadedScenes() {
    std::vector<LoadedGltfScene> out;
    {
      std::lock_guard lock{loadedGltfDataMutex_};
      out = std::move(loadedGltfData_);
      loadedGltfData_.clear();
    }
    return out;
  }
};
