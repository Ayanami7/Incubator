#include "runtime/render/vulkan/vulkan_scene_target.h"

#include <array>

#include "runtime/core/error/error.h"
#include "runtime/render/vulkan/vulkan_device.h"

namespace Incubator::Render::Vulkan
{
    namespace
    {
        void checkVk(VkResult result, const char* operation)
        {
            if (result != VK_SUCCESS)
            {
                throw Error::Exception(Error::Code::RENDER,
                                       fmt::format("{} failed with VkResult {}", operation, static_cast<int>(result)));
            }
        }

        VkImageMemoryBarrier2 makeImageBarrier(VkImage image, VkImageAspectFlags aspect, VkImageLayout oldLayout,
                                               VkImageLayout newLayout, VkPipelineStageFlags2 srcStage,
                                               VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                                               VkAccessFlags2 dstAccess) noexcept
        {
            VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barrier.srcStageMask = srcStage;
            barrier.srcAccessMask = srcAccess;
            barrier.dstStageMask = dstStage;
            barrier.dstAccessMask = dstAccess;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = {aspect, 0, 1, 0, 1};
            return barrier;
        }
    }  // namespace

    VulkanDisplayPass::VulkanDisplayPass(const VulkanDevice& device, VkFormat ldrFormat,
                                         const std::vector<uint32_t>& vertexShader,
                                         const std::vector<uint32_t>& fragmentShader)
        : device_(device), sampler_(device, {TextureFilter::Nearest, TextureFilter::Nearest, MipFilter::Nearest})
    {
        VkShaderModule vertexModule = VK_NULL_HANDLE;
        VkShaderModule fragmentModule = VK_NULL_HANDLE;
        try
        {
            VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            moduleInfo.codeSize = vertexShader.size() * sizeof(uint32_t);
            moduleInfo.pCode = vertexShader.data();
            checkVk(vkCreateShaderModule(device_.handle(), &moduleInfo, nullptr, &vertexModule),
                    "vkCreateShaderModule(display.vert)");
            moduleInfo.codeSize = fragmentShader.size() * sizeof(uint32_t);
            moduleInfo.pCode = fragmentShader.data();
            checkVk(vkCreateShaderModule(device_.handle(), &moduleInfo, nullptr, &fragmentModule),
                    "vkCreateShaderModule(display.frag)");

            const VkDescriptorSetLayoutBinding binding{
                0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo setLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            setLayoutInfo.bindingCount = 1;
            setLayoutInfo.pBindings = &binding;
            checkVk(vkCreateDescriptorSetLayout(device_.handle(), &setLayoutInfo, nullptr, &setLayout_),
                    "vkCreateDescriptorSetLayout(display)");

            const VkPushConstantRange pushRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(VulkanDisplayParams)};
            VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layoutInfo.setLayoutCount = 1;
            layoutInfo.pSetLayouts = &setLayout_;
            layoutInfo.pushConstantRangeCount = 1;
            layoutInfo.pPushConstantRanges = &pushRange;
            checkVk(vkCreatePipelineLayout(device_.handle(), &layoutInfo, nullptr, &pipelineLayout_),
                    "vkCreatePipelineLayout(display)");

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

            VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo inputAssembly{
                VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewport.viewportCount = 1;
            viewport.scissorCount = 1;
            constexpr std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
            dynamic.pDynamicStates = dynamicStates.data();
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode = VK_CULL_MODE_NONE;
            raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            raster.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo depthStencil{
                VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            VkPipelineColorBlendAttachmentState blendAttachment{};
            blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            blend.attachmentCount = 1;
            blend.pAttachments = &blendAttachment;
            VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachmentFormats = &ldrFormat;

            VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            pipelineInfo.pNext = &rendering;
            pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
            pipelineInfo.pStages = stages.data();
            pipelineInfo.pVertexInputState = &vertexInput;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewport;
            pipelineInfo.pRasterizationState = &raster;
            pipelineInfo.pMultisampleState = &multisample;
            pipelineInfo.pDepthStencilState = &depthStencil;
            pipelineInfo.pColorBlendState = &blend;
            pipelineInfo.pDynamicState = &dynamic;
            pipelineInfo.layout = pipelineLayout_;
            checkVk(vkCreateGraphicsPipelines(device_.handle(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_),
                    "vkCreateGraphicsPipelines(display)");
        }
        catch (...)
        {
            if (pipeline_ != VK_NULL_HANDLE)
                vkDestroyPipeline(device_.handle(), pipeline_, nullptr);
            if (pipelineLayout_ != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device_.handle(), pipelineLayout_, nullptr);
            if (setLayout_ != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device_.handle(), setLayout_, nullptr);
            if (fragmentModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(device_.handle(), fragmentModule, nullptr);
            if (vertexModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(device_.handle(), vertexModule, nullptr);
            throw;
        }
        vkDestroyShaderModule(device_.handle(), fragmentModule, nullptr);
        vkDestroyShaderModule(device_.handle(), vertexModule, nullptr);
    }

    VulkanDisplayPass::~VulkanDisplayPass()
    {
        if (pipeline_ != VK_NULL_HANDLE)
            vkDestroyPipeline(device_.handle(), pipeline_, nullptr);
        if (pipelineLayout_ != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device_.handle(), pipelineLayout_, nullptr);
        if (setLayout_ != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device_.handle(), setLayout_, nullptr);
    }

    VulkanSceneTarget::VulkanSceneTarget(const VulkanDevice& device, VkExtent2D extent, VkFormat depthFormat,
                                         VkFormat colorFormat, bool dualTarget)
        : device_(device)
    {
        if (!extent.width || !extent.height)
            throw Error::Exception(Error::Code::RENDER, "Scene target extent must be positive");
        constexpr VkImageUsageFlags colorUsage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        hdrColor_ = std::make_shared<VulkanImage>(device, extent, dualTarget ? VK_FORMAT_R16G16B16A16_SFLOAT : colorFormat,
                                                  colorUsage, VK_IMAGE_ASPECT_COLOR_BIT);
        if (dualTarget)
            ldrColor_ = std::make_shared<VulkanImage>(device, extent, VK_FORMAT_R8G8B8A8_UNORM, colorUsage,
                                                      VK_IMAGE_ASPECT_COLOR_BIT);
        depth_ = std::make_shared<VulkanImage>(device, extent, depthFormat,
                                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    VulkanSceneTarget::~VulkanSceneTarget()
    {
        if (descriptorPool_ != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device_.handle(), descriptorPool_, nullptr);
    }

    void VulkanSceneTarget::ensureHdrDescriptor(const VulkanDisplayPass& displayPass)
    {
        if (hdrDescriptorSet_ != VK_NULL_HANDLE)
            return;
        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        checkVk(vkCreateDescriptorPool(device_.handle(), &poolInfo, nullptr, &descriptorPool_),
                "vkCreateDescriptorPool(sceneTarget)");
        const VkDescriptorSetLayout layout = displayPass.descriptorSetLayout();
        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = descriptorPool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &layout;
        checkVk(vkAllocateDescriptorSets(device_.handle(), &allocInfo, &hdrDescriptorSet_),
                "vkAllocateDescriptorSets(sceneTarget)");
        const VkDescriptorImageInfo imageInfo{
            displayPass.sampler(), hdrColor_->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = hdrDescriptorSet_;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device_.handle(), 1, &write, 0, nullptr);
    }

    void VulkanSceneTarget::record(VkCommandBuffer cmd, const VkClearColorValue& clear,
                                   const std::function<void(VkCommandBuffer)>& draw)
    {
        const std::array barriers = {
            makeImageBarrier(hdrColor_->handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                             initialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             initialized_ ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                             initialized_ ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
            makeImageBarrier(depth_->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                             initialized_ ? (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT)
                                          : VK_PIPELINE_STAGE_2_NONE,
                             initialized_ ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0,
                             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT)};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dep.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(cmd, &dep);

        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = hdrColor_->view();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = clear;
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depth_->view();
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {1, 0};
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea.extent = hdrColor_->extent();
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color;
        info.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &info);
        VkViewport viewport{0, 0, float(hdrColor_->extent().width), float(hdrColor_->extent().height), 0, 1};
        VkRect2D scissor{{0, 0}, hdrColor_->extent()};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        draw(cmd);
        vkCmdEndRendering(cmd);

        const auto toRead = makeImageBarrier(
            hdrColor_->handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toRead;
        vkCmdPipelineBarrier2(cmd, &dep);
        initialized_ = true;
    }

    void VulkanSceneTarget::record(VkCommandBuffer cmd, const VulkanDisplayPass& displayPass,
                                   const VulkanDisplayParams& displayParams,
                                   const std::function<void(VkCommandBuffer)>& drawScene)
    {
        if (!ldrColor_)
            throw Error::Exception(Error::Code::RENDER, "Display pass requires dual HDR/LDR scene target");
        ensureHdrDescriptor(displayPass);

        // 1. 线性 HDR 主场景 Pass：alpha 清零用于标记未覆盖背景。
        const std::array hdrBarriers = {
            makeImageBarrier(hdrColor_->handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                             initialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             initialized_ ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                             initialized_ ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
            makeImageBarrier(depth_->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                             initialized_ ? (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT)
                                          : VK_PIPELINE_STAGE_2_NONE,
                             initialized_ ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0,
                             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT)};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(hdrBarriers.size());
        dep.pImageMemoryBarriers = hdrBarriers.data();
        vkCmdPipelineBarrier2(cmd, &dep);

        VkRenderingAttachmentInfo hdrAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        hdrAttachment.imageView = hdrColor_->view();
        hdrAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        hdrAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        hdrAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        hdrAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthAttachment.imageView = depth_->view();
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil = {1, 0};
        VkRenderingInfo hdrInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        hdrInfo.renderArea.extent = hdrColor_->extent();
        hdrInfo.layerCount = 1;
        hdrInfo.colorAttachmentCount = 1;
        hdrInfo.pColorAttachments = &hdrAttachment;
        hdrInfo.pDepthAttachment = &depthAttachment;
        vkCmdBeginRendering(cmd, &hdrInfo);
        VkViewport viewport{0, 0, float(hdrColor_->extent().width), float(hdrColor_->extent().height), 0, 1};
        VkRect2D scissor{{0, 0}, hdrColor_->extent()};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        drawScene(cmd);
        vkCmdEndRendering(cmd);

        // 2. HDR 写入 -> 显示 Pass 采样；同时将 LDR 目标转换为色彩附件。
        const std::array displayBarriers = {
            makeImageBarrier(hdrColor_->handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
            makeImageBarrier(ldrColor_->handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                             initialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             initialized_ ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                             initialized_ ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT)};
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(displayBarriers.size());
        dep.pImageMemoryBarriers = displayBarriers.data();
        vkCmdPipelineBarrier2(cmd, &dep);

        VkRenderingAttachmentInfo ldrAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ldrAttachment.imageView = ldrColor_->view();
        ldrAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ldrAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ldrAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ldrInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ldrInfo.renderArea.extent = ldrColor_->extent();
        ldrInfo.layerCount = 1;
        ldrInfo.colorAttachmentCount = 1;
        ldrInfo.pColorAttachments = &ldrAttachment;
        vkCmdBeginRendering(cmd, &ldrInfo);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, displayPass.pipeline());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, displayPass.pipelineLayout(), 0, 1,
                                &hdrDescriptorSet_, 0, nullptr);
        vkCmdPushConstants(cmd, displayPass.pipelineLayout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(VulkanDisplayParams), &displayParams);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);

        // 3. LDR 写入 -> ImGui 片元采样。
        const auto ldrToShader = makeImageBarrier(
            ldrColor_->handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &ldrToShader;
        vkCmdPipelineBarrier2(cmd, &dep);
        initialized_ = true;
    }
}  // namespace Incubator::Render::Vulkan
