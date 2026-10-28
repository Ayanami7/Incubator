#pragma once
#include "runtime/render/vulkan/vulkan_command_list.h"
#include "runtime/render/vulkan/vulkan_image.h"
namespace Incubator::Render::Vulkan
{
    /// @brief 独立场景颜色及深度附件；使用者保证帧完成后才修改或释放。
    class VulkanSceneTarget
    {
    public:
        VulkanSceneTarget(const VulkanDevice& device, VkExtent2D extent, VkFormat depthFormat);
        void record(VkCommandBuffer commands, const VkClearColorValue& color,
                    const std::function<void(VkCommandBuffer)>& draw);
        const VulkanImage& color() const
        {
            return *color_;
        }

    private:
        std::shared_ptr<VulkanImage> color_, depth_;
        bool initialized_ = false;
    };
}  // namespace Incubator::Render::Vulkan
