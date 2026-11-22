#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            class VulkanDevice;

            class VulkanPipeline
            {
            public:
                /// @brief colorFormat 为 UNDEFINED 时仅绘制深度，忽略材质布局及片元 Shader；linearHdrOutput 为 true 时启用线性 HDR 片元输出特化。
                VulkanPipeline(const VulkanDevice& device, VkFormat colorFormat, VkFormat depthFormat,
                               VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout materialLayout,
                               const std::vector<uint32_t>& vertexShader, const std::vector<uint32_t>& fragmentShader,
                               bool linearHdrOutput = false);
                ~VulkanPipeline();

                VulkanPipeline(const VulkanPipeline&) = delete;
                VulkanPipeline& operator=(const VulkanPipeline&) = delete;

                VkPipeline handle() const noexcept;
                VkPipelineLayout layout() const noexcept;
                VkFormat colorFormat() const noexcept;

            private:
                const VulkanDevice& device_;
                VkPipelineLayout layout_ = VK_NULL_HANDLE;
                VkPipeline pipeline_ = VK_NULL_HANDLE;
                VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
                VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
