#pragma once

#include <vulkan/vulkan.h>

#include <functional>
#include <glm/mat4x4.hpp>
#include <span>

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            class VulkanPipeline;
            class VulkanSwapchain;

            struct VulkanDrawItem
            {
                VkBuffer vertexBuffer;
                VkBuffer indexBuffer;
                uint32_t indexCount;
                VkDescriptorSet materialSet;
                glm::mat4 model;
            };

            class VulkanCommandList
            {
            public:
                static void record(VkCommandBuffer commandBuffer, const VulkanSwapchain& swapchain, uint32_t imageIndex,
                                   const VkClearColorValue& clearColor, VkImage depthImage, VkImageView depthView,
                                   const std::function<void(VkCommandBuffer)>& recordScene);

                static void drawIndexed(VkCommandBuffer commandBuffer, const VulkanPipeline& pipeline,
                                        VkDescriptorSet frameSet, std::span<const VulkanDrawItem> draws);
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
