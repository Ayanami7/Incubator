#pragma once

#include <functional>

#include "runtime/render/vulkan/vulkan_image.h"

namespace Incubator::Render::Vulkan
{
    /// @brief 单帧槽独占的固定深度阴影图，使用者在重用或释放前等待对应 Fence。
    class VulkanShadowTarget
    {
    public:
        static constexpr uint32_t SIZE = 1024;

        explicit VulkanShadowTarget(const VulkanDevice& device);
        /// @brief 检查所选格式的附件、采样、回读及固定图像配置能力。
        /// @throws Error::Exception 能力不足时抛出 RENDER。
        static void validateSupport(VkFormat format, const VkFormatProperties& properties,
                                    const VkImageFormatProperties& imageProperties);
        /// @brief 清深度为 1，绘制后转入片元采样布局；空回调同样清除旧结果。
        void record(VkCommandBuffer commands, const std::function<void(VkCommandBuffer)>& draw);
        const VulkanImage& image() const
        {
            return *image_;
        }

    private:
        std::unique_ptr<VulkanImage> image_;
        bool initialized_ = false;
    };
}  // namespace Incubator::Render::Vulkan
