#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "runtime/render/vulkan/vulkan_command_list.h"
#include "runtime/render/vulkan/vulkan_image.h"
#include "runtime/render/vulkan/vulkan_sampler.h"

namespace Incubator::Render::Vulkan
{
    class VulkanDevice;

    /// @brief 显示 Pass 片元 Push Constants 布局（与 display.frag 对应）。
    struct VulkanDisplayParams
    {
        std::array<float, 4> clearColor{0.0f, 0.0f, 0.0f, 1.0f};
        uint32_t toneMapping = 1;
        float exposureScale = 1.0f;
        float bloomIntensity = 0.0f;
        float padding = 0.0f;
    };
    static_assert(sizeof(VulkanDisplayParams) == 32);

    /// @brief HDR Bloom 后处理 Pass 的执行参数与共享管线借用。
    struct VulkanBloomParams
    {
        bool enabled = false;
        float threshold = 1.0f;
        float intensity = 0.35f;
        const class VulkanDisplayPass* prefilterPass = nullptr;
        const class VulkanDisplayPass* downsamplePass = nullptr;
        const class VulkanDisplayPass* upsamplePass = nullptr;
    };

    /// @brief 持有全屏后处理/显示 Pass 共享管线、采样器与描述符布局。
    class VulkanDisplayPass
    {
    public:
        VulkanDisplayPass(const VulkanDevice& device, VkFormat outputFormat,
                          const std::vector<uint32_t>& vertexShader,
                          const std::vector<uint32_t>& fragmentShader,
                          SamplerDesc samplerDesc = {TextureFilter::Nearest, TextureFilter::Nearest,
                                                   MipFilter::Nearest},
                          VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                          uint32_t pushConstantSize = sizeof(VulkanDisplayParams),
                          uint32_t samplerCount = 1u, bool additiveBlend = false);
        ~VulkanDisplayPass();

        VulkanDisplayPass(const VulkanDisplayPass&) = delete;
        VulkanDisplayPass& operator=(const VulkanDisplayPass&) = delete;

        VkDescriptorSetLayout descriptorSetLayout() const noexcept { return setLayout_; }
        VkPipelineLayout pipelineLayout() const noexcept { return pipelineLayout_; }
        VkPipeline pipeline() const noexcept { return pipeline_; }
        VkSampler sampler() const noexcept { return sampler_.handle(); }
        /// @brief 返回多输入显示 Pass 未启用 Bloom 时的 1×1 黑色占位纹理。
        const VulkanImage& fallbackBloom() const noexcept { return *fallbackBloom_; }

    private:
        const VulkanDevice& device_;
        VulkanSampler sampler_;
        std::shared_ptr<VulkanImage> fallbackBloom_;
        VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
    };

    /// @brief 独立场景颜色及深度附件；使用者保证帧完成后才修改或释放。
    class VulkanSceneTarget
    {
    public:
        static constexpr uint32_t MAX_BLOOM_MIPS = 5u;

        /// @brief 创建双目标（线性 HDR + LDR）或单目标（由 colorFormat 指定）离屏场景目标。
        VulkanSceneTarget(const VulkanDevice& device, VkExtent2D extent, VkFormat depthFormat,
                          VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM, bool dualTarget = false);
        ~VulkanSceneTarget();

        VulkanSceneTarget(const VulkanSceneTarget&) = delete;
        VulkanSceneTarget& operator=(const VulkanSceneTarget&) = delete;

        /// @brief 单目标录制路径（供内部 HDR 回读复核使用）。
        void record(VkCommandBuffer commands, const VkClearColorValue& color,
                    const std::function<void(VkCommandBuffer)>& draw);

        /// @brief 双目标录制路径：先录制线性 HDR 主场景，再执行可选 Bloom 链路与全屏显示 Pass 输出到 LDR，可选继续 FXAA。
        void record(VkCommandBuffer commands, const VulkanDisplayPass& displayPass,
                    const VulkanDisplayParams& displayParams,
                    const std::function<void(VkCommandBuffer)>& drawScene,
                    const VulkanDisplayPass* fxaaPass = nullptr, const VulkanBloomParams& bloom = {});

        /// @brief 为已分配的离屏场景目标按需补充同尺寸 FXAA 输出图像。
        void ensureFxaaTarget();

        const VulkanImage& color(bool fxaa = false) const;
        const VulkanImage& hdrColor() const { return *hdrColor_; }

    private:
        void ensureDescriptorPool();
        void ensureSingleSamplerSet(const VulkanDisplayPass& pass, VkDescriptorSet& set, const VulkanImage& image);
        void ensureDisplaySet(const VulkanDisplayPass& pass, const VulkanImage& hdrImage, const VulkanImage& bloomImage);
        void ensureBloomMips(const VulkanBloomParams& bloom);

        const VulkanDevice& device_;
        std::shared_ptr<VulkanImage> hdrColor_;
        std::shared_ptr<VulkanImage> ldrColor_;
        std::shared_ptr<VulkanImage> fxaaColor_;
        std::shared_ptr<VulkanImage> depth_;
        std::vector<std::shared_ptr<VulkanImage>> bloomMips_;
        VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
        VkDescriptorSet hdrDescriptorSet_ = VK_NULL_HANDLE;
        VkImageView boundBloomView_ = VK_NULL_HANDLE;
        VkDescriptorSet ldrDescriptorSet_ = VK_NULL_HANDLE;
        VkDescriptorSet bloomPrefilterSet_ = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> bloomDownSets_;
        std::vector<VkDescriptorSet> bloomUpSets_;
        bool initialized_ = false;
        bool fxaaInitialized_ = false;
    };
}  // namespace Incubator::Render::Vulkan
