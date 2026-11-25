#pragma once

#include <vulkan/vulkan.h>

#include <array>
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

            /// @brief 与 Shader std140 GpuLocalLight 对齐的局部光源 GPU 数据。
            struct alignas(16) GpuLocalLight
            {
                glm::vec4 positionRange{};   ///< xyz 为世界空间位置，w 为衰减半径。
                glm::vec4 colorIntensity{};  ///< rgb 为线性颜色，a 为强度。
                glm::vec4 directionType{};   ///< xyz 为归一化传播方向，w 为类型（0=Point，1=Spot）。
                glm::vec4 coneParams{};      ///< x 为 cos(innerConeAngle)，y 为 cos(outerConeAngle)。
            };
            static_assert(std::is_standard_layout_v<GpuLocalLight>);
            static_assert(sizeof(GpuLocalLight) == 64);

            /// @brief 与场景和深度 Shader 的 std140 FrameData 对齐的帧参数。
            struct alignas(16) VulkanFrameUniform
            {
                glm::mat4 view;
                glm::mat4 projection;
                glm::vec4 lightDirectionIntensity;  ///< xyz 为单位传播方向，w 为强度。
                glm::vec4 lightColorAmbient;        ///< xyz 为线性 RGB，w 为环境系数。
                std::array<glm::mat4, MAX_SHADOW_CASCADES> cascadeViewProjections;  ///< 各级联光源矩阵。
                glm::vec4 cascadeSplits;            ///< 各级联在相机视图空间中的正远深度阈值。
                glm::vec4 shadowParams;             ///< x 为接收深度偏移，y 为阴影启用标记，z 为法线偏移，w 为级联数量。
                glm::vec4 shadowMeta;               ///< x 为最大阴影距离，y 为阴影分辨率，其余保留为零。
                glm::vec4 cameraPosition;           ///< 世界空间相机位置（xyz），由视图矩阵派生。
                glm::vec4 iblParams;                ///< x 为 IBL 启用标记，y 为预过滤环境最大 mip 级，其余为零。
                glm::uvec4 localLightMeta;          ///< x 为激活局部光源数量，其余保留为零。
                std::array<GpuLocalLight, MAX_LOCAL_LIGHTS> localLights;  ///< 激活局部光源列表（前 localLightMeta.x 项有效）。
            };
            static_assert(std::is_standard_layout_v<VulkanFrameUniform>);
            static_assert(offsetof(VulkanFrameUniform, view) == 0);
            static_assert(offsetof(VulkanFrameUniform, projection) == 64);
            static_assert(offsetof(VulkanFrameUniform, lightDirectionIntensity) == 128);
            static_assert(offsetof(VulkanFrameUniform, lightColorAmbient) == 144);
            static_assert(alignof(VulkanFrameUniform) == 16);
            static_assert(offsetof(VulkanFrameUniform, cascadeViewProjections) == 160);
            static_assert(offsetof(VulkanFrameUniform, cascadeSplits) == 416);
            static_assert(offsetof(VulkanFrameUniform, shadowParams) == 432);
            static_assert(offsetof(VulkanFrameUniform, shadowMeta) == 448);
            static_assert(offsetof(VulkanFrameUniform, cameraPosition) == 464);
            static_assert(offsetof(VulkanFrameUniform, iblParams) == 480);
            static_assert(offsetof(VulkanFrameUniform, localLightMeta) == 496);
            static_assert(offsetof(VulkanFrameUniform, localLights) == 512);
            static_assert(sizeof(VulkanFrameUniform) == 1536);

            /// @brief 校验光照并打包本次提交的相机、光照与 IBL 参数，非单位方向在此归一化。
            /// @throws Error::Exception 光照输入无效时抛出 RENDER。
            VulkanFrameUniform makeFrameUniform(const FrameInput& input, float maxPrefilteredLod = 0.0f);

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
                /// @brief 更新全部帧槽的 IBL 绑定；任一空指针均选择安全占位纹理。
                /// @note 调用前须等待所有帧槽 GPU 工作完成；外部图像须存活至改绑或绑定集销毁。
                void setIblImages(const VulkanImage* irradiance = nullptr, const VulkanImage* prefiltered = nullptr,
                                  const VulkanImage* brdfLut = nullptr);
                /// @brief 返回 1×1 线性平坦法线回退图像（{128, 128, 255, 255}）。
                const VulkanImage& normalFallback() const noexcept;
                /// @brief 返回 1×1 线性金属粗糙度回退图像（{255, 255, 255, 255}）。
                const VulkanImage& metallicRoughnessFallback() const noexcept;
                /// @brief 返回自发光采样使用的 1×1 白图。
                const VulkanImage& emissiveFallback() const noexcept { return metallicRoughnessFallback(); }

            private:
                const VulkanDevice& device_;
                VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
                VkDescriptorSetLayout materialLayout_ = VK_NULL_HANDLE;
                VkDescriptorPool pool_ = VK_NULL_HANDLE;
                std::vector<VkDescriptorSet> descriptorSets_;
                std::shared_ptr<VulkanImage> shadowFallback_;
                std::shared_ptr<VulkanImage> normalFallback_;
                std::shared_ptr<VulkanImage> metallicRoughnessFallback_;
                std::shared_ptr<VulkanImage> iblCubeFallback_;
                std::shared_ptr<VulkanImage> iblLutFallback_;
                VkSampler shadowSampler_ = VK_NULL_HANDLE;
                VkSampler iblCubeSampler_ = VK_NULL_HANDLE;
                VkSampler iblLutSampler_ = VK_NULL_HANDLE;
                std::vector<VkImageView> shadowViews_;
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
