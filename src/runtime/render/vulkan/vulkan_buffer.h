#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <memory>
#include <span>

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            class VulkanDevice;

            class VulkanBuffer
            {
            public:
                VulkanBuffer(const VulkanDevice& device, VkDeviceSize size, VkBufferUsageFlags usage,
                             VkMemoryPropertyFlags requiredMemory,
                             VkMemoryPropertyFlags preferredMemory = 0);
                ~VulkanBuffer();

                VulkanBuffer(const VulkanBuffer&) = delete;
                VulkanBuffer& operator=(const VulkanBuffer&) = delete;

                static std::shared_ptr<VulkanBuffer> uploadStatic(const VulkanDevice& device,
                                                                  std::span<const std::byte> data,
                                                                  VkBufferUsageFlags usage,
                                                                  VkPipelineStageFlags2 consumerStage,
                                                                  VkAccessFlags2 consumerAccess);

                void write(VkDeviceSize offset, std::span<const std::byte> data);
                VkBuffer handle() const noexcept;
                VkDeviceSize size() const noexcept;

            private:
                const VulkanDevice& device_;
                VkBuffer buffer_ = VK_NULL_HANDLE;
                VkDeviceMemory memory_ = VK_NULL_HANDLE;
                VkDeviceSize size_ = 0;
                VkDeviceSize allocationSize_ = 0;
                VkMemoryPropertyFlags memoryProperties_ = 0;
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
