#pragma once

#include <functional>
#include <span>

#include "runtime/render/renderer.h"
#include "runtime/render/vulkan/vulkan_command_list.h"

namespace Incubator::Render::Vulkan
{
    class VulkanDevice;
    class VulkanPipeline;
}  // namespace Incubator::Render::Vulkan

namespace Incubator::Render::Detail
{
    // Synchronous internal review access. The callback borrows actual Renderer GPU resources;
    // attachment creation/readback remain in test support, outside the application API.
    struct RendererReviewAccess
    {
        using Record = std::function<void(const Vulkan::VulkanDevice&, const Vulkan::VulkanPipeline&, VkDescriptorSet,
                                          std::span<const Vulkan::VulkanDrawItem>)>;
        static void withFrame(Renderer& renderer, const FrameInput& input, const Record& record);
    };
}  // namespace Incubator::Render::Detail
