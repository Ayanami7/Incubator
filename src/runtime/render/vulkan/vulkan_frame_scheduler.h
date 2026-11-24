#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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

            /// @brief 按图形队列时间戳有效位与周期换算耗时（毫秒），正确处理计数器回绕。
            inline std::optional<double> computeTimestampDurationMs(uint64_t beginTicks, uint64_t endTicks,
                                                                    uint32_t validBits,
                                                                    float timestampPeriodNs) noexcept
            {
                if (validBits == 0 || timestampPeriodNs <= 0.0f)
                    return std::nullopt;
                const uint64_t mask = validBits >= 64 ? ~0ULL : ((1ULL << validBits) - 1ULL);
                const uint64_t maskedBegin = beginTicks & mask;
                const uint64_t maskedEnd = endTicks & mask;
                const uint64_t deltaTicks = (maskedEnd >= maskedBegin)
                                                ? (maskedEnd - maskedBegin)
                                                : ((mask - maskedBegin) + 1ULL + maskedEnd);
                const double nanoseconds =
                    static_cast<double>(deltaTicks) * static_cast<double>(timestampPeriodNs);
                return nanoseconds / 1'000'000.0;
            }

            /// @brief 离屏视口提交绑定的绘制计数输入。
            struct ViewportSampleInput
            {
                uint32_t sceneDrawCount = 0;
                uint32_t shadowDrawCount = 0;
            };

            /// @brief 帧槽 Fence 完成后读取到的离屏视口样本。
            struct CompletedViewportSample
            {
                std::optional<double> gpuTimeMs;
                uint32_t sceneDrawCount = 0;
                uint32_t shadowDrawCount = 0;
            };

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
                bool supportsTimestamps() const noexcept;
                const std::optional<CompletedViewportSample>& lastCompletedViewportSample() const noexcept;
                uint64_t completedViewportSampleCount() const noexcept;
                bool renderAndPresent(const VulkanSwapchain& swapchain, const VkClearColorValue& clearColor,
                                      const std::function<void(VkCommandBuffer)>& recordScene,
                                      std::span<const std::shared_ptr<void>> inFlightResources = {},
                                      const std::function<void(VkCommandBuffer)>& beforeScene = {},
                                      const std::optional<ViewportSampleInput>& viewportSample = std::nullopt);

            private:
                struct FrameSlot
                {
                    VkCommandPool commandPool = VK_NULL_HANDLE;
                    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
                    VkSemaphore imageAvailable = VK_NULL_HANDLE;
                    VkFence completionFence = VK_NULL_HANDLE;
                    VkQueryPool timestampPool = VK_NULL_HANDLE;
                    bool viewportStatsPending = false;
                    uint32_t pendingSceneDrawCount = 0;
                    uint32_t pendingShadowDrawCount = 0;
                    std::vector<std::shared_ptr<void>> inFlightResources;
                    std::shared_ptr<VulkanImage> depthAttachment;
                };

                void consumeFrameViewportSample(FrameSlot& frame);
                void destroySwapchainSync() noexcept;
                const VulkanDevice& device_;
                bool supportsTimestamps_ = false;
                std::optional<CompletedViewportSample> lastCompletedViewportSample_;
                uint64_t completedViewportSampleCount_ = 0;
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
