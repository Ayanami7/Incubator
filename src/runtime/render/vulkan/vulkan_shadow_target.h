#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "runtime/render/vulkan/vulkan_image.h"

namespace Incubator::Render::Vulkan
{
    /// @brief 单帧槽独占的 2D Array 深度阴影图，按级联层提供独立附件视图，重用或释放前须等待对应 Fence。
    class VulkanShadowTarget
    {
    public:
        static constexpr uint32_t SIZE = 1024;

        explicit VulkanShadowTarget(const VulkanDevice& device, uint32_t resolution = SIZE, uint32_t layerCount = 1u);
        ~VulkanShadowTarget();

        VulkanShadowTarget(const VulkanShadowTarget&) = delete;
        VulkanShadowTarget& operator=(const VulkanShadowTarget&) = delete;

        /// @brief 检查所选格式的附件、采样、回读及指定分辨率/级联层数的图像能力。
        /// @throws Error::Exception 能力不足时抛出 RENDER。
        static void validateSupport(VkFormat format, const VkFormatProperties& properties,
                                    const VkImageFormatProperties& imageProperties, uint32_t resolution = SIZE,
                                    uint32_t layerCount = 1u);
        /// @brief 逐级联层清深度为 1 并绘制，完成后整张 2D Array 图像转入片元采样布局；空回调同样清零旧结果。
        void record(VkCommandBuffer commands, const std::function<void(VkCommandBuffer, uint32_t)>& drawCascade);

        uint32_t resolution() const noexcept
        {
            return resolution_;
        }
        uint32_t layerCount() const noexcept
        {
            return layerCount_;
        }
        const VulkanImage& image() const noexcept
        {
            return *image_;
        }

    private:
        const VulkanDevice& device_;
        uint32_t resolution_ = SIZE;
        uint32_t layerCount_ = 1u;
        std::unique_ptr<VulkanImage> image_;
        std::vector<VkImageView> layerViews_;
        bool initialized_ = false;
    };
}  // namespace Incubator::Render::Vulkan
