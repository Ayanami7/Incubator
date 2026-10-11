#include "runtime/render/vulkan/vulkan_sampler.h"

#include "runtime/core/error/error.h"
#include "runtime/render/vulkan/vulkan_device.h"

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            VulkanSampler::VulkanSampler(const VulkanDevice& device) : device_(device)
            {
                VkSamplerCreateInfo samplerInfo{};
                samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
                samplerInfo.magFilter = VK_FILTER_NEAREST;
                samplerInfo.minFilter = VK_FILTER_NEAREST;
                samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
                samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
                samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
                samplerInfo.maxAnisotropy = 1.0f;
                samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
                samplerInfo.minLod = 0.0f;
                samplerInfo.maxLod = 0.0f;
                samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
                samplerInfo.unnormalizedCoordinates = VK_FALSE;
                const VkResult result = vkCreateSampler(device_.handle(), &samplerInfo, nullptr, &sampler_);
                if (result != VK_SUCCESS)
                {
                    throw Error::Exception(Error::Code::RENDER,
                                           fmt::format("vkCreateSampler failed with VkResult {}",
                                                       static_cast<int>(result)));
                }
            }

            VulkanSampler::~VulkanSampler()
            {
                if (sampler_ != VK_NULL_HANDLE)
                    vkDestroySampler(device_.handle(), sampler_, nullptr);
            }

            VkSampler VulkanSampler::handle() const noexcept
            {
                return sampler_;
            }
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
