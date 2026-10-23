#include "runtime/render/vulkan/vulkan_sampler.h"

#include "runtime/core/error/error.h"
#include "runtime/render/vulkan/vulkan_device.h"

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            VulkanSampler::VulkanSampler(const VulkanDevice& device, SamplerDesc desc, uint32_t mipLevels)
                : device_(device)
            {
                const auto filter = [](TextureFilter value) {
                    switch (value)
                    {
                        case TextureFilter::Nearest:
                            return VK_FILTER_NEAREST;
                        case TextureFilter::Linear:
                            return VK_FILTER_LINEAR;
                    }
                    throw Error::Exception(Error::Code::RENDER, "Invalid texture filter");
                };
                if (mipLevels == 0 || (desc.mipFilter != MipFilter::BaseLevel && desc.mipFilter != MipFilter::Nearest &&
                                       desc.mipFilter != MipFilter::Linear))
                    throw Error::Exception(Error::Code::RENDER, "Invalid mip sampling description");
                VkSamplerCreateInfo samplerInfo{};
                samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
                samplerInfo.magFilter = filter(desc.magFilter);
                samplerInfo.minFilter = filter(desc.minFilter);
                const bool baseLevelOnly = desc.mipFilter == MipFilter::BaseLevel || mipLevels == 1;
                samplerInfo.mipmapMode = !baseLevelOnly && desc.mipFilter == MipFilter::Linear
                                             ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                             : VK_SAMPLER_MIPMAP_MODE_NEAREST;
                samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
                samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
                samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
                samplerInfo.maxAnisotropy = 1.0f;
                samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
                samplerInfo.minLod = 0.0f;
                // maxLod=0 always selects magFilter. With nearest mip selection, 0.25 still selects
                // only level zero while allowing independent min/mag filters (Vulkan sampler spec).
                samplerInfo.maxLod = baseLevelOnly ? (desc.minFilter == desc.magFilter ? 0.0f : 0.25f)
                                                   : static_cast<float>(mipLevels - 1);
                samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
                samplerInfo.unnormalizedCoordinates = VK_FALSE;
                const VkResult result = vkCreateSampler(device_.handle(), &samplerInfo, nullptr, &sampler_);
                if (result != VK_SUCCESS)
                {
                    throw Error::Exception(Error::Code::RENDER, fmt::format("vkCreateSampler failed with VkResult {}",
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
