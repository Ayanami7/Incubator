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
                /// @brief 支持纯顶点深度、镂空深度和颜色输出，按调用方指定光栅剔除与透明混合方式。
                VulkanPipeline(const VulkanDevice& device, VkFormat colorFormat, VkFormat depthFormat,
                               VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout materialLayout,
                               const std::vector<uint32_t>& vertexShader, const std::vector<uint32_t>& fragmentShader,
                               bool linearHdrOutput = false, VkCullModeFlags cullMode = VK_CULL_MODE_NONE,
                               bool alphaBlend = false);

                /// @brief 创建无顶点输入管线（用于环境天空盒：仅绑定帧布局并按指定深度比较运行）。
                VulkanPipeline(const VulkanDevice& device, VkFormat colorFormat, VkFormat depthFormat,
                               VkDescriptorSetLayout frameLayout, const std::vector<uint32_t>& vertexShader,
                               const std::vector<uint32_t>& fragmentShader, VkCompareOp depthCompareOp,
                               bool depthWriteEnable, VkCullModeFlags cullMode = VK_CULL_MODE_NONE);
                ~VulkanPipeline();

                VulkanPipeline(const VulkanPipeline&) = delete;
                VulkanPipeline& operator=(const VulkanPipeline&) = delete;

                VkPipeline handle() const noexcept;
                VkPipelineLayout layout() const noexcept;
                VkFormat colorFormat() const noexcept;

            private:
                void initPipeline(VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout materialLayout,
                                  const std::vector<uint32_t>& vertexShader,
                                  const std::vector<uint32_t>& fragmentShader, bool linearHdrOutput,
                                  VkCullModeFlags cullMode, bool alphaBlend, bool vertexless,
                                  VkCompareOp depthCompareOp, bool depthWriteEnable);

                const VulkanDevice& device_;
                VkPipelineLayout layout_ = VK_NULL_HANDLE;
                VkPipeline pipeline_ = VK_NULL_HANDLE;
                VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
                VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
