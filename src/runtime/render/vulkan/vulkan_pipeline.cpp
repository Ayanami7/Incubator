#include "runtime/render/vulkan/vulkan_pipeline.h"

#include <array>
#include <iterator>

#include "runtime/core/error/error.h"
#include "runtime/render/vulkan/vulkan_device.h"

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            namespace
            {
                void checkVk(VkResult result, const char* operation)
                {
                    if (result != VK_SUCCESS)
                    {
                        throw Error::Exception(Error::Code::RENDER,
                                               fmt::format("{} failed with VkResult {}", operation,
                                                           static_cast<int>(result)));
                    }
                }
            }  // namespace

            VulkanPipeline::VulkanPipeline(const VulkanDevice& device, VkFormat colorFormat,
                                           const std::vector<uint32_t>& vertexShader,
                                           const std::vector<uint32_t>& fragmentShader)
                : device_(device), colorFormat_(colorFormat)
            {
                VkShaderModule vertexModule = VK_NULL_HANDLE;
                VkShaderModule fragmentModule = VK_NULL_HANDLE;
                try
                {
                    VkShaderModuleCreateInfo moduleInfo{};
                    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                    moduleInfo.codeSize = vertexShader.size() * sizeof(uint32_t);
                    moduleInfo.pCode = vertexShader.data();
                    checkVk(vkCreateShaderModule(device_.handle(), &moduleInfo, nullptr, &vertexModule),
                            "vkCreateShaderModule(vertex)");
                    moduleInfo.codeSize = fragmentShader.size() * sizeof(uint32_t);
                    moduleInfo.pCode = fragmentShader.data();
                    checkVk(vkCreateShaderModule(device_.handle(), &moduleInfo, nullptr, &fragmentModule),
                            "vkCreateShaderModule(fragment)");

                    VkPipelineLayoutCreateInfo layoutInfo{};
                    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
                    checkVk(vkCreatePipelineLayout(device_.handle(), &layoutInfo, nullptr, &layout_),
                            "vkCreatePipelineLayout");

                    const std::array<VkPipelineShaderStageCreateInfo, 2> stages = [&] {
                        std::array<VkPipelineShaderStageCreateInfo, 2> result{};
                        result[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                        result[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
                        result[0].module = vertexModule;
                        result[0].pName = "main";
                        result[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                        result[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
                        result[1].module = fragmentModule;
                        result[1].pName = "main";
                        return result;
                    }();

                    VkPipelineVertexInputStateCreateInfo vertexInput{};
                    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
                    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
                    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
                    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                    VkPipelineViewportStateCreateInfo viewport{};
                    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
                    viewport.viewportCount = 1;
                    viewport.scissorCount = 1;
                    constexpr VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
                    VkPipelineDynamicStateCreateInfo dynamic{};
                    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
                    dynamic.dynamicStateCount = static_cast<uint32_t>(std::size(dynamicStates));
                    dynamic.pDynamicStates = dynamicStates;
                    VkPipelineRasterizationStateCreateInfo rasterization{};
                    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
                    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
                    rasterization.cullMode = VK_CULL_MODE_NONE;
                    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                    rasterization.lineWidth = 1.0f;
                    VkPipelineMultisampleStateCreateInfo multisample{};
                    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
                    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
                    VkPipelineColorBlendAttachmentState blendAttachment{};
                    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
                    VkPipelineColorBlendStateCreateInfo blend{};
                    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
                    blend.attachmentCount = 1;
                    blend.pAttachments = &blendAttachment;
                    VkPipelineRenderingCreateInfo rendering{};
                    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
                    rendering.colorAttachmentCount = 1;
                    rendering.pColorAttachmentFormats = &colorFormat_;

                    VkGraphicsPipelineCreateInfo pipelineInfo{};
                    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
                    pipelineInfo.pNext = &rendering;
                    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
                    pipelineInfo.pStages = stages.data();
                    pipelineInfo.pVertexInputState = &vertexInput;
                    pipelineInfo.pInputAssemblyState = &inputAssembly;
                    pipelineInfo.pViewportState = &viewport;
                    pipelineInfo.pRasterizationState = &rasterization;
                    pipelineInfo.pMultisampleState = &multisample;
                    pipelineInfo.pColorBlendState = &blend;
                    pipelineInfo.pDynamicState = &dynamic;
                    pipelineInfo.layout = layout_;
                    checkVk(vkCreateGraphicsPipelines(device_.handle(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                                      &pipeline_),
                            "vkCreateGraphicsPipelines");
                }
                catch (...)
                {
                    if (pipeline_ != VK_NULL_HANDLE)
                        vkDestroyPipeline(device_.handle(), pipeline_, nullptr);
                    if (layout_ != VK_NULL_HANDLE)
                        vkDestroyPipelineLayout(device_.handle(), layout_, nullptr);
                    if (fragmentModule != VK_NULL_HANDLE)
                        vkDestroyShaderModule(device_.handle(), fragmentModule, nullptr);
                    if (vertexModule != VK_NULL_HANDLE)
                        vkDestroyShaderModule(device_.handle(), vertexModule, nullptr);
                    throw;
                }
                vkDestroyShaderModule(device_.handle(), fragmentModule, nullptr);
                vkDestroyShaderModule(device_.handle(), vertexModule, nullptr);
            }

            VulkanPipeline::~VulkanPipeline()
            {
                if (pipeline_ != VK_NULL_HANDLE)
                    vkDestroyPipeline(device_.handle(), pipeline_, nullptr);
                if (layout_ != VK_NULL_HANDLE)
                    vkDestroyPipelineLayout(device_.handle(), layout_, nullptr);
            }

            VkPipeline VulkanPipeline::handle() const noexcept
            {
                return pipeline_;
            }

            VkPipelineLayout VulkanPipeline::layout() const noexcept
            {
                return layout_;
            }

            VkFormat VulkanPipeline::colorFormat() const noexcept
            {
                return colorFormat_;
            }
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
