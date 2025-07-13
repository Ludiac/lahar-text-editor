module;

#include "macros.hpp"
#include "primitive_types.hpp"

export module vulkan_app:VulkanPipeline;

import :Types;
import vulkan_hpp;
import std;

[[nodiscard]] std::expected<std::vector<u32>, std::string>
readSpirvFile(const std::string &filename) NOEXCEPT {
  std::ifstream file(filename, std::ios::ate | std::ios::binary);
  if (!file.is_open()) {
    return std::unexpected("Failed to open file: " + filename);
  }

  std::size_t fileSize = file.tellg();
  if (fileSize == 0) {
    file.close();
    return std::unexpected("File is empty: " + filename);
  }
  std::vector<u32> buffer(fileSize / sizeof(u32));

  file.seekg(0);
  file.read(reinterpret_cast<char *>(buffer.data()), static_cast<i64>(fileSize));
  file.close();

  return buffer;
}

[[nodiscard]] std::expected<vk::raii::ShaderModule, std::string>
createShaderModule(const vk::raii::Device &device, const std::vector<u32> &spirvCode) NOEXCEPT {
  if (spirvCode.empty()) {
    return std::unexpected("Cannot create shader module from empty SPIR-V code.");
  }
  auto createInfo = vk::ShaderModuleCreateInfo{
      .codeSize = spirvCode.size() * sizeof(u32),
      .pCode = spirvCode.data(),
  };
  auto shaderModuleResult = device.createShaderModule(createInfo);
  if (!shaderModuleResult) {
    return std::unexpected("Failed to create shader module: " +
                           vk::to_string(shaderModuleResult.error()));
  }
  return std::move(shaderModuleResult.value());
}

export [[nodiscard]] std::expected<vk::raii::ShaderModule, std::string>
createShaderModuleFromFile(const vk::raii::Device &device, const std::string &filename) NOEXCEPT {
  auto spirvCodeResult = readSpirvFile(filename);
  if (!spirvCodeResult) {
    return std::unexpected(spirvCodeResult.error());
  }

  return createShaderModule(device, *spirvCodeResult);
}

export struct VulkanPipeline {
  vk::raii::PipelineLayout pipelineLayout{nullptr};
  vk::raii::Pipeline pipeline{nullptr};

  [[nodiscard]] std::expected<void, std::string>
  createPipelineLayout(const vk::raii::Device &device,
                       const std::vector<vk::DescriptorSetLayout> &descriptorSetLayouts,
                       const std::vector<vk::PushConstantRange> &pushConstantRanges) NOEXCEPT {
    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{
        .setLayoutCount = static_cast<u32>(descriptorSetLayouts.size()),
        .pSetLayouts = descriptorSetLayouts.data(),
    };

    if (!pushConstantRanges.empty()) {
      pipelineLayoutInfo.pushConstantRangeCount = static_cast<u32>(pushConstantRanges.size());
      pipelineLayoutInfo.pPushConstantRanges = pushConstantRanges.data();
    }

    auto layoutResult = device.createPipelineLayout(pipelineLayoutInfo);
    if (!layoutResult) {
      return std::unexpected("Failed to create pipeline layout: " +
                             vk::to_string(layoutResult.error()));
    }
    pipelineLayout = std::move(layoutResult.value());
    return {};
  }

  [[nodiscard]] std::expected<void, std::string> createGraphicsPipeline(
      const vk::raii::Device &device, const vk::raii::PipelineCache &pipelineCache,
      std::vector<vk::PipelineShaderStageCreateInfo> shaderStages,
      vk::PipelineVertexInputStateCreateInfo vertexInputInfo,
      vk::PipelineInputAssemblyStateCreateInfo inputAssembly,
      const vk::raii::RenderPass &renderPass,
      vk::PipelineColorBlendAttachmentState *colorBlendAttachmentOverride = nullptr,
      vk::PipelineDepthStencilStateCreateInfo *depthStencilStateOverride = nullptr) NOEXCEPT {
    vk::PipelineViewportStateCreateInfo viewportState{
        .viewportCount = 1,
        .scissorCount = 1,
    };

    vk::PipelineRasterizationStateCreateInfo rasterizer{
        .depthClampEnable = vk::False,
        .rasterizerDiscardEnable = vk::False,
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eBack,
        .frontFace = vk::FrontFace::eClockwise,
        .depthBiasEnable = vk::False,
        .lineWidth = 1.0,
    };

    vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
        .sampleShadingEnable = vk::False,
    };

    vk::PipelineDepthStencilStateCreateInfo defaultDepthStencilState{
        .depthTestEnable = vk::True,
        .depthWriteEnable = vk::True,
        .depthCompareOp = vk::CompareOp::eLess,
    };

    vk::PipelineColorBlendAttachmentState defaultColorBlendAttachment{
        .blendEnable = vk::False,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };

    vk::PipelineColorBlendStateCreateInfo colorBlending{
        .logicOpEnable = vk::False,
        .attachmentCount = 1,
        .pAttachments = (colorBlendAttachmentOverride != nullptr) ? colorBlendAttachmentOverride
                                                                  : &defaultColorBlendAttachment,
    };

    std::vector<vk::DynamicState> dynamicStates = {
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };

    vk::PipelineDynamicStateCreateInfo dynamicState{
        .dynamicStateCount = static_cast<u32>(dynamicStates.size()),
        .pDynamicStates = dynamicStates.data(),
    };

    vk::GraphicsPipelineCreateInfo pipelineInfo{
        .stageCount = static_cast<u32>(shaderStages.size()),
        .pStages = shaderStages.data(),
        .pVertexInputState = &vertexInputInfo,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = (depthStencilStateOverride != nullptr) ? depthStencilStateOverride
                                                                     : &defaultDepthStencilState,
        .pColorBlendState = &colorBlending,
        .pDynamicState = &dynamicState,
        .layout = *pipelineLayout,
        .renderPass = renderPass,
        .subpass = 0,
    };

    auto pipelineResult = device.createGraphicsPipeline(pipelineCache, pipelineInfo);

    if (!pipelineResult) {
      return std::unexpected("Failed to create graphics pipeline: " +
                             vk::to_string(pipelineResult.error()));
    }
    pipeline = std::move(pipelineResult.value());
    std::println("Graphics pipeline created successfully!");
    return {};
  }
};
