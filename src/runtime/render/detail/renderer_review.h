#pragma once

#include <cstddef>
#include <functional>
#include <span>

#include "runtime/render/renderer.h"
#include "runtime/render/vulkan/vulkan_command_list.h"

namespace Incubator::Render::Vulkan
{
    class VulkanDevice;
    class VulkanPipeline;
    class VulkanImage;
}  // namespace Incubator::Render::Vulkan

namespace Incubator::Render::Detail
{
    /// @brief 提供同步内部复核访问。
    /// @note 回调借用 Renderer 实际持有的 GPU 资源；附件创建和回读由测试支持代码完成，不属于应用接口。
    struct RendererReviewAccess
    {
        struct ResourceCounts
        {
            size_t meshes;
            size_t textures;
            size_t materials;
        };
        /// @brief 统计实际存活对象，包含直接创建、资源组、材质依赖和在途持有；不等待或回收。
        /// @note 不包含内部帧目标、白图、字体、Shader 或 Pipeline。
        static ResourceCounts resourceCounts(Renderer& renderer);
        /// @brief 等待全部已提交帧的 Fence 并释放帧槽持有，不等待整个设备空闲。
        static void waitForFrames(Renderer& renderer);
        static void withSceneOutput(
            Renderer& renderer, const SceneOutput& output,
            const std::function<void(const Vulkan::VulkanDevice&, const Vulkan::VulkanImage&)>& record);
        /// @brief 等待在途帧后借用对应场景槽的阴影深度图，供回读或受控采样复核。
        static void withShadowOutput(
            Renderer& renderer, const SceneOutput& output,
            const std::function<void(const Vulkan::VulkanDevice&, const Vulkan::VulkanImage&)>& record);
        using Record = std::function<void(const Vulkan::VulkanDevice&, const Vulkan::VulkanPipeline&, VkDescriptorSet,
                                          std::span<const Vulkan::VulkanDrawItem>)>;
        /// @brief 使用指定帧和当前 Renderer 资源执行复核回调。
        /// @param renderer 被复核的 Renderer。
        /// @param input 待提交帧输入。
        /// @param record 访问当前 GPU 资源的回调。
        static void withFrame(Renderer& renderer, const FrameInput& input, const Record& record);
    };
}  // namespace Incubator::Render::Detail
