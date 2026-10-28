#include "runtime/render/vulkan/vulkan_shadow_target.h"

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/render/vulkan/vulkan_device.h"

namespace Incubator::Render::Vulkan
{
    namespace
    {
        constexpr VkImageUsageFlags USAGE =
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        constexpr VkFormatFeatureFlags FEATURES = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                  VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;

        bool supports(VkFormat format, const VkFormatProperties& properties, const VkImageFormatProperties& image)
        {
            constexpr auto size = VulkanShadowTarget::SIZE;
            return (format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM) &&
                   (properties.optimalTilingFeatures & FEATURES) == FEATURES && image.maxExtent.width >= size &&
                   image.maxExtent.height >= size && image.maxExtent.depth >= 1 && image.maxMipLevels >= 1 &&
                   image.maxArrayLayers >= 1 && (image.sampleCounts & VK_SAMPLE_COUNT_1_BIT) &&
                   image.maxResourceSize >= VkDeviceSize(size) * size * (format == VK_FORMAT_D16_UNORM ? 2 : 4);
        }
    }  // namespace

    void VulkanShadowTarget::validateSupport(VkFormat format, const VkFormatProperties& properties,
                                             const VkImageFormatProperties& image)
    {
        if (!supports(format, properties, image))
            throw Error::Exception(
                Error::Code::RENDER,
                fmt::format("Shadow depth format {} does not support sampled {}x{} depth attachment usage {}",
                            int(format), SIZE, SIZE, USAGE));
    }

    VulkanShadowTarget::VulkanShadowTarget(const VulkanDevice& device)
    {
        VkFormat selected = VK_FORMAT_UNDEFINED;
        for (const auto format : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM})
        {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(device.physicalDevice(), format, &properties);
            if ((properties.optimalTilingFeatures & FEATURES) != FEATURES)
                continue;
            VkImageFormatProperties image{};
            if (vkGetPhysicalDeviceImageFormatProperties(device.physicalDevice(), format, VK_IMAGE_TYPE_2D,
                                                         VK_IMAGE_TILING_OPTIMAL, USAGE, 0, &image) != VK_SUCCESS)
                continue;
            if (!supports(format, properties, image))
                continue;
            selected = format;
            break;
        }
        if (selected == VK_FORMAT_UNDEFINED)
            throw Error::Exception(Error::Code::RENDER,
                                   "No sampled shadow depth format supports 1024x1024 attachments");
        image_ =
            std::make_unique<VulkanImage>(device, VkExtent2D{SIZE, SIZE}, selected, USAGE, VK_IMAGE_ASPECT_DEPTH_BIT);
        Log::Registry::get("render.vulkan")
            .debug("Created shadow target: {}x{}, depth format {}", SIZE, SIZE, int(selected));
    }

    void VulkanShadowTarget::record(VkCommandBuffer commands, const std::function<void(VkCommandBuffer)>& draw)
    {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = initialized_ ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = initialized_ ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0;
        barrier.dstStageMask =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        barrier.dstAccessMask =
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = initialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image_->handle();
        barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commands, &dependency);
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = image_->view();
        depth.imageLayout = barrier.newLayout;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {1, 0};
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = image_->extent();
        rendering.layerCount = 1;
        rendering.pDepthAttachment = &depth;
        vkCmdBeginRendering(commands, &rendering);
        const VkViewport viewport{0, 0, float(SIZE), float(SIZE), 0, 1};
        const VkRect2D scissor{{0, 0}, {SIZE, SIZE}};
        vkCmdSetViewport(commands, 0, 1, &viewport);
        vkCmdSetScissor(commands, 0, 1, &scissor);
        draw(commands);
        vkCmdEndRendering(commands);
        barrier.srcStageMask =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(commands, &dependency);
        initialized_ = true;
    }
}  // namespace Incubator::Render::Vulkan
