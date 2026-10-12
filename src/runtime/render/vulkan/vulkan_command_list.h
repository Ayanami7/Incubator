#pragma once

#include <vulkan/vulkan.h>

#include <functional>

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            class VulkanPipeline;
            class VulkanBuffer;
            class VulkanSwapchain;

            class VulkanCommandList
            {
            public:
                static void recordIndexed(VkCommandBuffer commandBuffer, const VulkanSwapchain& swapchain,
                                          uint32_t imageIndex, const VkClearColorValue& clearColor,
                                          const VulkanPipeline& pipeline, const VulkanBuffer& vertexBuffer,
                                          const VulkanBuffer& indexBuffer, VkImage depthImage,
                                          VkImageView depthView,
                                          VkDescriptorSet descriptorSet, uint32_t indexCount,
                                          const std::function<void(VkCommandBuffer)>& recordUi);
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
