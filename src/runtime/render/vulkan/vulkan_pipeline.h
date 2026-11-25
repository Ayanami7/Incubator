#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <vector>

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            class VulkanDevice;

            /// @brief 3D 调试辅助线 GPU 顶点格式（位置与线性 RGBA）。
            struct VulkanDebugLineVertex
            {
                glm::vec3 position;
                glm::vec4 color;
            };
            static_assert(sizeof(VulkanDebugLineVertex) == 28);

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

                /// @brief 创建 3D 调试辅助线管线（LINE_LIST，开启深度测试但关闭深度写入，支持 Alpha 混合）。
                VulkanPipeline(const VulkanDevice& device, VkFormat colorFormat, VkFormat depthFormat,
                               VkDescriptorSetLayout frameLayout, const std::vector<uint32_t>& vertexShader,
                               const std::vector<uint32_t>& fragmentShader);
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
                                  VkCullModeFlags cullMode, bool alphaBlend, bool vertexless, bool debugLines,
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
