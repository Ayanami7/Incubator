#pragma once

#include <vulkan/vulkan.h>

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            class VulkanDevice;

            class VulkanSampler
            {
            public:
                explicit VulkanSampler(const VulkanDevice& device);
                ~VulkanSampler();

                VulkanSampler(const VulkanSampler&) = delete;
                VulkanSampler& operator=(const VulkanSampler&) = delete;

                VkSampler handle() const noexcept;

            private:
                const VulkanDevice& device_;
                VkSampler sampler_ = VK_NULL_HANDLE;
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
