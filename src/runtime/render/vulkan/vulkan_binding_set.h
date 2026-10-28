#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <glm/vec4.hpp>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

#include "runtime/render/frame_input.h"

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            class VulkanBuffer;
            class VulkanDevice;
            class VulkanImage;

            /// @brief 与场景和深度 Shader 的 std140 FrameData 对齐的帧参数。
            struct alignas(16) VulkanFrameUniform
            {
                glm::mat4 view;
                glm::mat4 projection;
                glm::vec4 lightDirectionIntensity;  ///< xyz 为单位传播方向，w 为强度。
                glm::vec4 lightColorAmbient;        ///< xyz 为线性 RGB，w 为环境系数。
                glm::mat4 shadowViewProjection;     ///< 固定光源投影与观察矩阵的乘积。
                glm::vec4 shadowParams;             ///< x 为接收深度偏移，y 为阴影启用标记，其余为零。
            };
            static_assert(std::is_standard_layout_v<VulkanFrameUniform>);
            static_assert(offsetof(VulkanFrameUniform, view) == 0);
            static_assert(offsetof(VulkanFrameUniform, projection) == 64);
            static_assert(offsetof(VulkanFrameUniform, lightDirectionIntensity) == 128);
            static_assert(offsetof(VulkanFrameUniform, lightColorAmbient) == 144);
            static_assert(alignof(VulkanFrameUniform) == 16);
            static_assert(offsetof(VulkanFrameUniform, shadowViewProjection) == 160);
            static_assert(offsetof(VulkanFrameUniform, shadowParams) == 224);
            static_assert(sizeof(VulkanFrameUniform) == 240);

            /// @brief 校验光照并打包本次提交的相机与光照值，非单位方向在此归一化。
            /// @throws Error::Exception 光照输入无效时抛出 RENDER。
            VulkanFrameUniform makeFrameUniform(const FrameInput& input);

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
                /// @brief 更新阴影绑定；空指针选择已初始化的全亮白图。
                /// @note 调用前须完成对应帧槽的 GPU 工作；外部图像须存活至改绑或绑定集销毁。
                void setShadowImage(uint32_t frameIndex, const VulkanImage* image = nullptr);

            private:
                const VulkanDevice& device_;
                VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
                VkDescriptorSetLayout materialLayout_ = VK_NULL_HANDLE;
                VkDescriptorPool pool_ = VK_NULL_HANDLE;
                std::vector<VkDescriptorSet> descriptorSets_;
                std::shared_ptr<VulkanImage> shadowFallback_;
                VkSampler shadowSampler_ = VK_NULL_HANDLE;
                std::vector<VkImageView> shadowViews_;
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
