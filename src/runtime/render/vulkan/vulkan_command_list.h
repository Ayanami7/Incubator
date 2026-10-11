#pragma once

#include <vulkan/vulkan.h>

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            class VulkanPipeline;
            class VulkanSwapchain;

            class VulkanCommandList
            {
            public:
                static void recordTriangle(VkCommandBuffer commandBuffer, const VulkanSwapchain& swapchain,
                                           uint32_t imageIndex, const VkClearColorValue& clearColor,
                                           const VulkanPipeline& pipeline);
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
