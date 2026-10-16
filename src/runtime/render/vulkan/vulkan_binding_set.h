#pragma once

#include <vulkan/vulkan.h>

#include <memory>
#include <span>
#include <vector>

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            class VulkanBuffer;
            class VulkanDevice;

            class VulkanBindingSet
            {
            public:
                VulkanBindingSet(const VulkanDevice& device,
                                 std::span<const std::shared_ptr<VulkanBuffer>> uniformBuffers);
                ~VulkanBindingSet();

                VulkanBindingSet(const VulkanBindingSet&) = delete;
                VulkanBindingSet& operator=(const VulkanBindingSet&) = delete;

                VkDescriptorSetLayout layout() const noexcept;
                VkDescriptorSetLayout materialLayout() const noexcept;
                VkDescriptorSet descriptorSet(uint32_t frameIndex) const noexcept;

            private:
                const VulkanDevice& device_;
                VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
                VkDescriptorSetLayout materialLayout_ = VK_NULL_HANDLE;
                VkDescriptorPool pool_ = VK_NULL_HANDLE;
                std::vector<VkDescriptorSet> descriptorSets_;
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
