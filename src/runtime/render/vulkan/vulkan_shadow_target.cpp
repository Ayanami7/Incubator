#include "runtime/render/vulkan/vulkan_shadow_target.h"

#include <bit>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/render/shadow.h"
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

        bool supports(VkFormat format, const VkFormatProperties& properties, const VkImageFormatProperties& image,
                      uint32_t resolution, uint32_t layerCount)
        {
            return (format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM) &&
                   (properties.optimalTilingFeatures & FEATURES) == FEATURES && resolution > 0u && layerCount > 0u &&
                   image.maxExtent.width >= resolution && image.maxExtent.height >= resolution &&
                   image.maxExtent.depth >= 1 && image.maxMipLevels >= 1 && image.maxArrayLayers >= layerCount &&
                   (image.sampleCounts & VK_SAMPLE_COUNT_1_BIT) &&
                   image.maxResourceSize >= VkDeviceSize(resolution) * resolution * layerCount *
                                                (format == VK_FORMAT_D16_UNORM ? 2 : 4);
        }
    }  // namespace

    void VulkanShadowTarget::validateSupport(VkFormat format, const VkFormatProperties& properties,
                                             const VkImageFormatProperties& image, uint32_t resolution,
                                             uint32_t layerCount)
    {
        if (!supports(format, properties, image, resolution, layerCount))
            throw Error::Exception(
                Error::Code::RENDER,
                fmt::format("Shadow depth format {} does not support sampled {}x{} ({} layers) depth attachment usage {}",
                            int(format), resolution, resolution, layerCount, USAGE));
    }

    VulkanShadowTarget::VulkanShadowTarget(const VulkanDevice& device, uint32_t resolution, uint32_t layerCount)
        : device_(device), resolution_(resolution), layerCount_(layerCount)
    {
        if (!std::has_single_bit(resolution_) || resolution_ < 512u || resolution_ > 4096u || layerCount_ == 0u ||
            layerCount_ > MAX_SHADOW_CASCADES)
            throw Error::Exception(Error::Code::RENDER,
                                   fmt::format("Invalid shadow target dimensions {}x{} ({} layers)", resolution_,
                                               resolution_, layerCount_));
        const uint32_t maxDim = device_.properties().limits.maxImageDimension2D;
        if (resolution_ > maxDim)
            throw Error::Exception(
                Error::Code::RENDER,
                fmt::format("Shadow resolution {} exceeds device maxImageDimension2D {}", resolution_, maxDim));

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
            if (!supports(format, properties, image, resolution_, layerCount_))
                continue;
            selected = format;
            break;
        }
        if (selected == VK_FORMAT_UNDEFINED)
            throw Error::Exception(
                Error::Code::RENDER,
                fmt::format("No sampled shadow depth format supports {}x{} ({} layers) attachments", resolution_,
                            resolution_, layerCount_));

        try
        {
            image_ = std::make_unique<VulkanImage>(device, VkExtent2D{resolution_, resolution_}, selected, USAGE,
                                                   VK_IMAGE_ASPECT_DEPTH_BIT, 1u, layerCount_, 0u,
                                                   VK_IMAGE_VIEW_TYPE_2D_ARRAY);
            layerViews_.reserve(layerCount_);
            for (uint32_t layer = 0; layer < layerCount_; ++layer)
            {
                VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                viewInfo.image = image_->handle();
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                viewInfo.format = selected;
                viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1};
                VkImageView view = VK_NULL_HANDLE;
                const VkResult result = vkCreateImageView(device_.handle(), &viewInfo, nullptr, &view);
                if (result != VK_SUCCESS)
                    throw Error::Exception(Error::Code::RENDER,
                                           fmt::format("vkCreateImageView(shadow layer {}) failed with VkResult {}",
                                                       layer, static_cast<int>(result)));
                layerViews_.push_back(view);
            }
        }
        catch (...)
        {
            for (VkImageView view : layerViews_)
                if (view != VK_NULL_HANDLE)
                    vkDestroyImageView(device_.handle(), view, nullptr);
            throw;
        }
        Log::Registry::get("render.vulkan")
            .debug("Created shadow target: {}x{} ({} layers), depth format {}", resolution_, resolution_, layerCount_,
                   int(selected));
    }

    VulkanShadowTarget::~VulkanShadowTarget()
    {
        for (VkImageView view : layerViews_)
            if (view != VK_NULL_HANDLE)
                vkDestroyImageView(device_.handle(), view, nullptr);
    }

    void VulkanShadowTarget::record(VkCommandBuffer commands,
                                    const std::function<void(VkCommandBuffer, uint32_t)>& drawCascade)
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
        barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layerCount_};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commands, &dependency);

        const VkViewport viewport{0, 0, float(resolution_), float(resolution_), 0, 1};
        const VkRect2D scissor{{0, 0}, {resolution_, resolution_}};
        for (uint32_t layer = 0; layer < layerCount_; ++layer)
        {
            VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            depth.imageView = layerViews_[layer];
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth.clearValue.depthStencil = {1, 0};
            VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
            rendering.renderArea.extent = image_->extent();
            rendering.layerCount = 1;
            rendering.pDepthAttachment = &depth;
            vkCmdBeginRendering(commands, &rendering);
            vkCmdSetViewport(commands, 0, 1, &viewport);
            vkCmdSetScissor(commands, 0, 1, &scissor);
            if (drawCascade)
                drawCascade(commands, layer);
            vkCmdEndRendering(commands);
        }

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
