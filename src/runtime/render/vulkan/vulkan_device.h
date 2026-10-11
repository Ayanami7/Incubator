#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>

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
            };

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
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
