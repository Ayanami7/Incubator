#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <vector>

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            class VulkanDevice;
            class VulkanSwapchain;

            class VulkanFrameScheduler
            {
            public:
                explicit VulkanFrameScheduler(const VulkanDevice& device);
                ~VulkanFrameScheduler();

                VulkanFrameScheduler(const VulkanFrameScheduler&) = delete;
                VulkanFrameScheduler& operator=(const VulkanFrameScheduler&) = delete;

                void initializeSwapchainImages(uint32_t imageCount);
                void waitForFrames();
                void waitForPresentation();
                bool renderAndPresent(const VulkanSwapchain& swapchain, const VkClearColorValue& clearColor);

            private:
                struct FrameSlot
                {
                    VkCommandPool commandPool = VK_NULL_HANDLE;
                    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
                    VkSemaphore imageAvailable = VK_NULL_HANDLE;
                    VkFence completionFence = VK_NULL_HANDLE;
                };

                void destroySwapchainSync() noexcept;
                void recordClear(const VulkanSwapchain& swapchain, uint32_t imageIndex,
                                const VkClearColorValue& clearColor, VkCommandBuffer commandBuffer);

                const VulkanDevice& device_;
                std::array<FrameSlot, 2> frames_{};
                std::vector<VkSemaphore> presentReady_;
                std::vector<VkFence> presentFences_;
                std::vector<bool> presentPending_;
                std::vector<VkFence> imageOwners_;
                uint32_t frameIndex_ = 0;
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
