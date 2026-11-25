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
                glm::vec4 tangentParams;     ///< x 为模型线性矩阵行列式符号（±1，无效时为 0），其余为零。
            };
            static_assert(std::is_standard_layout_v<VulkanObjectData>);
            static_assert(offsetof(VulkanObjectData, model) == 0);
            static_assert(offsetof(VulkanObjectData, normalColumns) == 64);
            static_assert(offsetof(VulkanObjectData, tangentParams) == 112);
            static_assert(sizeof(glm::vec4) == 16);
            static_assert(alignof(VulkanObjectData) == 16);
            static_assert(sizeof(VulkanObjectData) == 128);
            static_assert(sizeof(VulkanObjectData) <= 128);

            /// @brief 与深度阴影顶点 Shader 的 ObjectData 对齐的逐物体级联常量。
            struct alignas(16) VulkanShadowObjectData
            {
                glm::mat4 model;
                uint32_t cascadeIndex = 0u;
                uint32_t padding[3]{};
            };
            static_assert(std::is_standard_layout_v<VulkanShadowObjectData>);
            static_assert(offsetof(VulkanShadowObjectData, model) == 0);
            static_assert(offsetof(VulkanShadowObjectData, cascadeIndex) == 64);
            static_assert(sizeof(VulkanShadowObjectData) == 80);

            struct VulkanDrawItem
            {
                VkBuffer vertexBuffer;
                VkBuffer indexBuffer;
                uint32_t indexCount;
                VkDescriptorSet materialSet;
                VulkanObjectData object;
                const VulkanPipeline* pipeline = nullptr;  ///< 指定该绘制项所用的管线变体。
                VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            };

            /// @brief 深度绘制借用网格、管线和模型矩阵，镂空变体还借用材质描述符集。
            struct VulkanShadowDrawItem
            {
                VkBuffer vertexBuffer;
                VkBuffer indexBuffer;
                uint32_t indexCount;
                glm::mat4 model;
                const VulkanPipeline* pipeline = nullptr;
                VkDescriptorSet materialSet;
                bool doubleSided;
                VkFrontFace frontFace;
            };

            class VulkanCommandList
            {
            public:
                static void record(VkCommandBuffer commandBuffer, const VulkanSwapchain& swapchain, uint32_t imageIndex,
                                   const VkClearColorValue& clearColor, VkImage depthImage, VkImageView depthView,
                                   const std::function<void(VkCommandBuffer)>& recordScene,
                                   const std::function<void(VkCommandBuffer)>& beforeScene = {},
                                   VkQueryPool timestampPool = VK_NULL_HANDLE);

                static void drawIndexed(VkCommandBuffer commandBuffer, const VulkanPipeline& pipeline,
                                        VkDescriptorSet frameSet, std::span<const VulkanDrawItem> draws);
                /// @brief 绑定光源帧参数并录制指定级联层的实心或镂空深度绘制。
                static void drawShadow(VkCommandBuffer commandBuffer, VkDescriptorSet frameSet,
                                       std::span<const VulkanShadowDrawItem> draws, uint32_t cascadeIndex = 0u);
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
