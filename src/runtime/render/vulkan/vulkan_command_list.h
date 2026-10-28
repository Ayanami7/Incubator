#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <functional>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <span>
#include <type_traits>

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            class VulkanPipeline;
            class VulkanSwapchain;

            /// @brief 与顶点 Shader 的 ObjectData 对齐的逐物体常量。
            struct alignas(16) VulkanObjectData
            {
                glm::mat4 model;
                glm::vec4 normalColumns[3];  ///< 逆转置矩阵的列；每列按 16 字节对齐，w 为零。
            };
            static_assert(std::is_standard_layout_v<VulkanObjectData>);
            static_assert(offsetof(VulkanObjectData, model) == 0);
            static_assert(offsetof(VulkanObjectData, normalColumns) == 64);
            static_assert(sizeof(glm::vec4) == 16);
            static_assert(alignof(VulkanObjectData) == 16);
            static_assert(sizeof(VulkanObjectData) == 112);
            static_assert(sizeof(VulkanObjectData) <= 128);

            struct VulkanDrawItem
            {
                VkBuffer vertexBuffer;
                VkBuffer indexBuffer;
                uint32_t indexCount;
                VkDescriptorSet materialSet;
                VulkanObjectData object;
            };

            /// @brief 深度绘制只借用网格缓冲区和模型矩阵，不解析材质或法线。
            struct VulkanShadowDrawItem
            {
                VkBuffer vertexBuffer;
                VkBuffer indexBuffer;
                uint32_t indexCount;
                glm::mat4 model;
            };

            class VulkanCommandList
            {
            public:
                static void record(VkCommandBuffer commandBuffer, const VulkanSwapchain& swapchain, uint32_t imageIndex,
                                   const VkClearColorValue& clearColor, VkImage depthImage, VkImageView depthView,
                                   const std::function<void(VkCommandBuffer)>& recordScene,
                                   const std::function<void(VkCommandBuffer)>& beforeScene = {});

                static void drawIndexed(VkCommandBuffer commandBuffer, const VulkanPipeline& pipeline,
                                        VkDescriptorSet frameSet, std::span<const VulkanDrawItem> draws);
                /// @brief 绑定光源帧参数并录制双面深度绘制。
                static void drawShadow(VkCommandBuffer commandBuffer, const VulkanPipeline& pipeline,
                                       VkDescriptorSet frameSet, std::span<const VulkanShadowDrawItem> draws);
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
