#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            class VulkanDevice;
            class VulkanImage;
            class VulkanSwapchain;

            class VulkanFrameScheduler
            {
            public:
                static constexpr uint32_t FRAME_COUNT = 2;

                explicit VulkanFrameScheduler(const VulkanDevice& device);
                ~VulkanFrameScheduler();

                VulkanFrameScheduler(const VulkanFrameScheduler&) = delete;
                VulkanFrameScheduler& operator=(const VulkanFrameScheduler&) = delete;

                void initializeSwapchainImages(uint32_t imageCount, VkExtent2D extent, VkFormat depthFormat);
                void waitForFrames();
                void waitForCurrentFrame();
                void waitForPresentation();
                uint32_t currentFrameIndex() const noexcept;
                bool renderAndPresent(const VulkanSwapchain& swapchain, const VkClearColorValue& clearColor,
                                      const std::function<void(VkCommandBuffer)>& recordScene,
                                      std::span<const std::shared_ptr<void>> inFlightResources = {},
                                      const std::function<void(VkCommandBuffer)>& beforeScene = {});

            private:
                struct FrameSlot
                {
                    VkCommandPool commandPool = VK_NULL_HANDLE;
                    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
                    VkSemaphore imageAvailable = VK_NULL_HANDLE;
                    VkFence completionFence = VK_NULL_HANDLE;
                    std::vector<std::shared_ptr<void>> inFlightResources;
                    std::shared_ptr<VulkanImage> depthAttachment;
                };

                void destroySwapchainSync() noexcept;
                const VulkanDevice& device_;
                std::array<FrameSlot, FRAME_COUNT> frames_{};
                std::vector<VkSemaphore> presentReady_;
                std::vector<VkFence> presentFences_;
                std::vector<bool> presentPending_;
                std::vector<VkFence> imageOwners_;
                uint32_t frameIndex_ = 0;
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
