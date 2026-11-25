#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            class GlfwSurface;
            class VulkanInstance;

            struct QueueFamilyIndices
            {
                uint32_t graphics = VK_QUEUE_FAMILY_IGNORED;
                uint32_t present = VK_QUEUE_FAMILY_IGNORED;
                uint32_t graphicsTimestampValidBits = 0;
            };

            /// @brief 参与物理设备择优排序的纯数据摘要。
            struct PhysicalDeviceCandidate
            {
                VkPhysicalDeviceType deviceType = VK_PHYSICAL_DEVICE_TYPE_OTHER;
                uint32_t apiVersion = 0;
                bool unifiedGraphicsPresentQueue = false;
                bool swapchainMaintenance1 = false;
                bool graphicsTimestamps = false;
            };

            /// @brief 从已通过资格校验的候选设备中按确定性优先级选出最优下标。
            /// @note 优先级依次为：设备类型（独显 > 核显 > 虚拟 > CPU > 其他）、图形与呈现同队列族、
            /// 支持 swapchainMaintenance1、支持图形时间戳、API 版本更高；同分保留先枚举项。
            std::optional<size_t> selectBestPhysicalDevice(std::span<const PhysicalDeviceCandidate> candidates) noexcept;

            class VulkanDevice
            {
            public:
                VulkanDevice(const VulkanInstance& instance, const GlfwSurface& surface);
                ~VulkanDevice();

                VulkanDevice(const VulkanDevice&) = delete;
                VulkanDevice& operator=(const VulkanDevice&) = delete;

                VkPhysicalDevice physicalDevice() const noexcept;
                VkDevice handle() const noexcept;
                VkQueue graphicsQueue() const noexcept;
                VkQueue presentQueue() const noexcept;
                const QueueFamilyIndices& queueFamilies() const noexcept;
                const VkPhysicalDeviceProperties& properties() const noexcept;
                /// @brief 判断选定图形队列族是否支持有效时间戳与非零计时周期。
                bool supportsGraphicsTimestamps() const noexcept;
                /// @brief 判断当前逻辑设备是否已启用 VK_EXT_swapchain_maintenance1。
                bool supportsSwapchainMaintenance1() const noexcept;
                uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags required,
                                        VkMemoryPropertyFlags preferred,
                                        VkMemoryPropertyFlags* selectedProperties = nullptr) const;
                VkFormat findDepthFormat() const;
                void executeImmediate(const std::function<void(VkCommandBuffer)>& recordCommands) const;

            private:
                VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
                VkDevice device_ = VK_NULL_HANDLE;
                VkQueue graphicsQueue_ = VK_NULL_HANDLE;
                VkQueue presentQueue_ = VK_NULL_HANDLE;
                QueueFamilyIndices queueFamilies_;
                VkPhysicalDeviceProperties properties_{};
                VkPhysicalDeviceMemoryProperties memoryProperties_{};
                bool swapchainMaintenance1Enabled_ = false;
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
