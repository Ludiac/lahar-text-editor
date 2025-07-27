module;

#include "macros.hpp"
#include "primitive_types.hpp"

export module vulkan_app:TwoDEngine;

import vulkan_hpp;
import std;
import BS.thread_pool;

import :VulkanDevice;
import :VulkanPipeline;
import :TextSystem;
import :UISystem;
import :Types;
import :utils;
import :TextEditor;
import :TextView;
import :text;
import :TextWidget;

export class TwoDEngine {
private:
  VulkanDevice &m_device;
  u32 m_frameCount;
  BS::thread_pool<> &m_thread_pool;

  // 2D Rendering Resources
  VulkanPipeline m_textPipeline;
  VulkanPipeline m_uiPipeline;

  // 2D Systems - To be managed by the 2D Engine
  TextSystem m_textSystem;
  UISystem m_uiSystem;

  std::vector<std::future<std::expected<Font *, std::string>>> m_fontLoadFutures;
  std::mutex m_fontFuturesMutex;

  std::vector<std::unique_ptr<TextEditor>> m_textEditors;
  std::vector<TextWidget> m_textWidgets;
  std::vector<Font *> m_registeredFonts;
  size_t m_activeWidgetIndex = 0;

  [[nodiscard]] std::expected<void, std::string>
  createGraphicsPipelines(const vk::raii::RenderPass &renderPass,
                          const vk::raii::PipelineCache &pipelineCache) {
    auto textVertShaderResult =
        createShaderModuleFromFile(m_device.logical(), "shaders/text_vert.spv");
    if (!textVertShaderResult) {
      return std::unexpected(textVertShaderResult.error());
    }
    auto textFragShaderResult =
        createShaderModuleFromFile(m_device.logical(), "shaders/text_frag.spv");
    if (!textFragShaderResult) {
      return std::unexpected(textFragShaderResult.error());
    }
    auto textVertShader = std::move(*textVertShaderResult);
    auto textFragShader = std::move(*textFragShaderResult);

    vk::raii::DescriptorSetLayout textSetLayout{nullptr};
    std::vector<vk::DescriptorSetLayoutBinding> textBindings = {
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eFragment}};
    vk::DescriptorSetLayoutCreateInfo textLayoutInfo{
        .bindingCount = static_cast<u32>(textBindings.size()), .pBindings = textBindings.data()};

    auto textSetLayoutResult = m_device.logical().createDescriptorSetLayout(textLayoutInfo);
    if (!textSetLayoutResult) {
      return std::unexpected("Failed to create text descriptor set layout.");
    }

    textSetLayout = std::move(textSetLayoutResult.value());

    auto uiVertShaderResult = createShaderModuleFromFile(m_device.logical(), "shaders/ui_vert.spv");
    if (!uiVertShaderResult) {
      return std::unexpected(uiVertShaderResult.error());
    }
    auto uiFragShaderResult = createShaderModuleFromFile(m_device.logical(), "shaders/ui_frag.spv");
    if (!uiFragShaderResult) {
      return std::unexpected(uiFragShaderResult.error());
    }
    auto uiVertShader = std::move(*uiVertShaderResult);
    auto uiFragShader = std::move(*uiFragShaderResult);

    vk::raii::DescriptorSetLayout instanceSetLayout{nullptr};
    std::vector<vk::DescriptorSetLayoutBinding> instanceBindings = {
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eStorageBufferDynamic,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eVertex}};
    vk::DescriptorSetLayoutCreateInfo instanceLayoutInfo{
        .bindingCount = static_cast<u32>(instanceBindings.size()),
        .pBindings = instanceBindings.data()};

    auto instanceSetLayoutResult = m_device.logical().createDescriptorSetLayout(instanceLayoutInfo);
    if (!instanceSetLayoutResult) {
      return std::unexpected("Failed to create instance descriptor set layout.");
    }
    instanceSetLayout = std::move(instanceSetLayoutResult.value());

    vk::PushConstantRange textPushConstantRange{.stageFlags = vk::ShaderStageFlagBits::eVertex |
                                                              vk::ShaderStageFlagBits::eFragment,
                                                .offset = 0,
                                                .size = sizeof(std::array<std::byte, 128>)};
    std::vector<vk::DescriptorSetLayout> textLayouts = {*instanceSetLayout, *textSetLayout};
    auto textPipelineLayoutResult = m_textPipeline.createPipelineLayout(
        m_device.logical(), textLayouts, {textPushConstantRange});
    if (!textPipelineLayoutResult) {
      return std::unexpected(textPipelineLayoutResult.error());
    }

    vk::PushConstantRange uiPushConstantRange{.stageFlags = vk::ShaderStageFlagBits::eVertex,
                                              .offset = 0,
                                              .size = sizeof(std::array<std::byte, 128>)};
    std::vector<vk::DescriptorSetLayout> uiLayouts = {*instanceSetLayout};
    auto uiPipelineLayoutResult =
        m_uiPipeline.createPipelineLayout(m_device.logical(), uiLayouts, {uiPushConstantRange});
    if (!uiPipelineLayoutResult) {
      return std::unexpected(uiPipelineLayoutResult.error());
    }

    std::vector<vk::PipelineShaderStageCreateInfo> textShaderStages = {
        {.stage = vk::ShaderStageFlagBits::eVertex, .module = *textVertShader, .pName = "main"},
        {.stage = vk::ShaderStageFlagBits::eFragment, .module = *textFragShader, .pName = "main"}};
    std::vector<vk::PipelineShaderStageCreateInfo> uiShaderStages = {
        {.stage = vk::ShaderStageFlagBits::eVertex, .module = *uiVertShader, .pName = "main"},
        {.stage = vk::ShaderStageFlagBits::eFragment, .module = *uiFragShader, .pName = "main"}};

    vk::VertexInputBindingDescription textBindingDesc{
        .binding = 0, .stride = sizeof(TextQuadVertex), .inputRate = vk::VertexInputRate::eVertex};
    std::array<vk::VertexInputAttributeDescription, 2> textAttrDesc;
    textAttrDesc[0] = {
        .location = 0, .binding = 0, .format = vk::Format::eR32G32Sfloat, .offset = 0};
    textAttrDesc[1] = {.location = 1,
                       .binding = 0,
                       .format = vk::Format::eR32G32Sfloat,
                       .offset = offsetof(TextQuadVertex, uv)};

    vk::PipelineVertexInputStateCreateInfo vertexInputInfo{
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &textBindingDesc,
        .vertexAttributeDescriptionCount = 2,
        .pVertexAttributeDescriptions = textAttrDesc.data()};
    vk::PipelineInputAssemblyStateCreateInfo inputAssembly{
        .topology = vk::PrimitiveTopology::eTriangleList};
    vk::PipelineColorBlendAttachmentState blendAttachment{
        .blendEnable = vk::True,
        .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
        .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eOne,
        .dstAlphaBlendFactor = vk::BlendFactor::eZero,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
    vk::PipelineDepthStencilStateCreateInfo depthStencil{.depthTestEnable = vk::False,
                                                         .depthWriteEnable = vk::False};

    auto textGraphicsPipelineResult = m_textPipeline.createGraphicsPipeline(
        m_device.logical(), pipelineCache, textShaderStages, vertexInputInfo, inputAssembly,
        renderPass, &blendAttachment, &depthStencil);
    if (!textGraphicsPipelineResult) {
      return std::unexpected(textGraphicsPipelineResult.error());
    }
    auto uiGraphicsPipelineResult = m_uiPipeline.createGraphicsPipeline(
        m_device.logical(), pipelineCache, uiShaderStages, vertexInputInfo, inputAssembly,
        renderPass, &blendAttachment, &depthStencil);
    if (!uiGraphicsPipelineResult) {
      return std::unexpected(uiGraphicsPipelineResult.error());
    }

    registerFont("../../assets/fonts/Inconsolata/InconsolataNerdFontMono-Regular.ttf",
                 textSetLayout);

    return {}; // Success
  }

public:
  // UI State
  i32 fontSizeMultiplier{0};
  TextToggles textToggles;
  //

  TwoDEngine(VulkanDevice &device, u32 frameCount, BS::thread_pool<> &threadPool)
      : m_device(device), m_frameCount(frameCount), m_thread_pool(threadPool) {}

  TextEditor *getActiveTextEditor() {
    if (m_textWidgets.empty())
      return nullptr;
    return m_textWidgets[m_activeWidgetIndex].getEditor();
  }

  void cycleActiveWidgetFocus() {
    if (m_textWidgets.empty())
      return;
    m_textWidgets[m_activeWidgetIndex].setActive(false);
    m_activeWidgetIndex = (m_activeWidgetIndex + 1) % m_textWidgets.size();
    m_textWidgets[m_activeWidgetIndex].setActive(true);
  }

  std::expected<void, std::string> initialize(const vk::raii::RenderPass &renderPass,
                                              const vk::raii::PipelineCache &pipelineCache) {
    auto textSystemExpected = TextSystem::create(m_device, m_frameCount, m_device.descriptorPool());
    if (!textSystemExpected) {
      return std::unexpected(
          std::format("textSystem creation failed: {}", textSystemExpected.error()));
    }
    m_textSystem = std::move(textSystemExpected.value());

    auto uiSystemExpected = UISystem::create(m_device, m_frameCount, m_device.descriptorPool());
    if (!uiSystemExpected) {
      return std::unexpected(std::format("uiSystem creation failed: {}", uiSystemExpected.error()));
    }
    m_uiSystem = std::move(uiSystemExpected.value());

    EXPECTED_VOID(createGraphicsPipelines(renderPass, pipelineCache));

    return {};
  }

  void update(u32 frameIndex, float deltaTime, vk::Extent2D swapchainExtent) {
    // In a more advanced engine, you might update animations or other dynamic UI elements here.
    m_textSystem.beginFrame();
    m_uiSystem.beginFrame();
  }

  void draw(vk::raii::CommandBuffer &cmd, u32 frameIndex, vk::Extent2D swapchainExtent) {
    RenderQueue renderQueue;

    for (auto &widget : m_textWidgets) {
      widget.draw(m_uiSystem, m_textSystem);
    }

    m_textSystem.prepareBatches(renderQueue, m_textPipeline, frameIndex, swapchainExtent,
                                textToggles);
    m_uiSystem.prepareBatches(renderQueue, m_uiPipeline, frameIndex, swapchainExtent);

    processRenderQueue(
        cmd, renderQueue); // Assuming processRenderQueue is a free function or part of the engine
  }

  void onSwapchainRecreated(u32 newFrameCount) {
    m_frameCount = newFrameCount;
    // You may need to resize or recreate resources within TextSystem and UISystem
  }

  void processFontLoads() {
    std::lock_guard lock(m_fontFuturesMutex);
    for (auto it = m_fontLoadFutures.begin(); it != m_fontLoadFutures.end();) {
      if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto res = it->get();
        if (res) {
          Font *font = *res;
          m_registeredFonts.emplace_back(font);
          if (m_textWidgets.empty() && (font != nullptr)) {
            m_textEditors.push_back(std::make_unique<TextEditor>("Hello from TextWidget 1!"));
            m_textWidgets.emplace_back(m_textEditors.back().get(), font,
                                       Quad{.position = {100, 100},
                                            .size = {400, 500},
                                            .color = {0.1, 0.1, 0.1, 1.0},
                                            .zLayer = 0.0});
            m_textWidgets.back().setActive(true);

            m_textEditors.push_back(
                std::make_unique<TextEditor>("This is the second widget.\n\nCool."));
            m_textWidgets.emplace_back(m_textEditors.back().get(), font,
                                       Quad{.position = {600, 100},
                                            .size = {400, 300},
                                            .color = {0.1, 0.1, 0.2, 1.0},
                                            .zLayer = 0.0});
          }
        } else {
          std::println("failed to load font asynchronously: {}", res.error());
        }
        it = m_fontLoadFutures.erase(it);
      }
    }
  }

  void registerFont(const std::string &fontPath, const vk::raii::DescriptorSetLayout &layout) {
    std::lock_guard lock(m_fontFuturesMutex);
    m_fontLoadFutures.emplace_back(m_thread_pool.submit_task([this, fontPath]() {
      // This is a bit of a hack, we need to get the layout from the pipeline
      // but the pipeline is created after the text system.
      // For now, we assume the layout is known.
      vk::raii::DescriptorSetLayout textSetLayout{nullptr};
      std::vector<vk::DescriptorSetLayoutBinding> textBindings = {
          {.binding = 0,
           .descriptorType = vk::DescriptorType::eCombinedImageSampler,
           .descriptorCount = 1,
           .stageFlags = vk::ShaderStageFlagBits::eFragment}};
      vk::DescriptorSetLayoutCreateInfo textLayoutInfo{
          .bindingCount = static_cast<u32>(textBindings.size()), .pBindings = textBindings.data()};
      textSetLayout = m_device.logical().createDescriptorSetLayout(textLayoutInfo).value();
      return m_textSystem.registerFont(fontPath, textSetLayout);
    }));
  }
};
