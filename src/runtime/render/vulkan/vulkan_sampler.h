#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

#include "runtime/render/sampler.h"

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
                explicit VulkanSampler(const VulkanDevice& device, SamplerDesc desc = {}, uint32_t mipLevels = 1);
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
