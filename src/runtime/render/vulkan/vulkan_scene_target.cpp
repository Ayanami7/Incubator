#include "runtime/render/vulkan/vulkan_scene_target.h"

#include "runtime/core/error/error.h"
namespace Incubator::Render::Vulkan
{
    VulkanSceneTarget::VulkanSceneTarget(const VulkanDevice& device, VkExtent2D extent, VkFormat depthFormat)
    {
        if (!extent.width || !extent.height)
            throw Error::Exception(Error::Code::RENDER, "Scene target extent must be positive");
        color_ = std::make_shared<VulkanImage>(
            device, extent, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
        depth_ = std::make_shared<VulkanImage>(device, extent, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                               VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    void VulkanSceneTarget::record(VkCommandBuffer cmd, const VkClearColorValue& clear,
                                   const std::function<void(VkCommandBuffer)>& draw)
    {
        VkImageMemoryBarrier2 barriers[2]{};
        for (auto& b : barriers)
        {
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.layerCount = 1;
        }
        auto& c = barriers[0];
        c.image = color_->handle();
        c.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.oldLayout = initialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        c.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        c.srcStageMask = initialized_ ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE;
        c.srcAccessMask = initialized_ ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0;
        c.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        c.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        auto& d = barriers[1];
        d.image = depth_->handle();
        d.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        d.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        d.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        d.srcStageMask =
            initialized_ ? VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
                         : VK_PIPELINE_STAGE_2_NONE;
        d.srcAccessMask = initialized_ ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0;
        d.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        d.dstAccessMask =
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 2;
        dep.pImageMemoryBarriers = barriers;
        vkCmdPipelineBarrier2(cmd, &dep);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = color_->view();
        color.imageLayout = c.newLayout;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = clear;
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depth_->view();
        depth.imageLayout = d.newLayout;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {1, 0};
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea.extent = color_->extent();
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color;
        info.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &info);
        VkViewport viewport{0, 0, float(color_->extent().width), float(color_->extent().height), 0, 1};
        VkRect2D scissor{{0, 0}, color_->extent()};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        draw(cmd);
        vkCmdEndRendering(cmd);
        c.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        c.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        c.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        c.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        c.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        c.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        dep.imageMemoryBarrierCount = 1;
        vkCmdPipelineBarrier2(cmd, &dep);
        initialized_ = true;
    }
}  // namespace Incubator::Render::Vulkan
