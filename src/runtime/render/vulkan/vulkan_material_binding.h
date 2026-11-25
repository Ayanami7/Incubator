#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <memory>

#include "runtime/render/material.h"

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            class VulkanBuffer;
            class VulkanDevice;
            class VulkanImage;
            class VulkanSampler;

            /// @brief 使用 Renderer 共享材质布局的不可变材质描述符集。
            class VulkanMaterialBinding
            {
            public:
                VulkanMaterialBinding(const VulkanDevice& device, VkDescriptorSetLayout layout,
                                      const VulkanImage& colorImage, const VulkanSampler& colorSampler,
                                      const VulkanImage& normalImage, const VulkanSampler& normalSampler,
                                      const VulkanImage& metallicRoughnessImage,
                                      const VulkanSampler& metallicRoughnessSampler,
                                      const VulkanImage& emissiveImage, const VulkanSampler& emissiveSampler,
                                      const MaterialDesc& description);
                ~VulkanMaterialBinding();

                VulkanMaterialBinding(const VulkanMaterialBinding&) = delete;
                VulkanMaterialBinding& operator=(const VulkanMaterialBinding&) = delete;

                VkDescriptorSet descriptorSet() const noexcept;

            private:
                const VulkanDevice& device_;
                std::unique_ptr<VulkanBuffer> uniform_;
                VkDescriptorPool pool_ = VK_NULL_HANDLE;
                VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
