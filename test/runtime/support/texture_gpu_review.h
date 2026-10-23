#pragma once

#include <imgui.h>

#include <cstring>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <vector>

#include "render_demo/scene_assets.h"
#include "render_demo/scene_mesh.h"
#include "runtime/platform/window/window.h"
#include "runtime/render/integration/imgui_renderer.h"
#include "runtime/render/vulkan/vulkan_binding_set.h"
#include "runtime/render/vulkan/vulkan_buffer.h"
#include "runtime/render/vulkan/vulkan_command_list.h"
#include "runtime/render/vulkan/vulkan_device.h"
#include "runtime/render/vulkan/vulkan_image.h"
#include "runtime/render/vulkan/vulkan_material_binding.h"
#include "runtime/render/vulkan/vulkan_pipeline.h"
#include "runtime/render/vulkan/vulkan_sampler.h"

namespace TextureGpuReview
{
    using namespace Incubator;
    using namespace Incubator::Render;
    using namespace Incubator::Render::Vulkan;

    inline void check(VkResult result)
    {
        if (result != VK_SUCCESS)
            throw Error::Exception(Error::Code::RENDER, fmt::format("GPU readback failed: {}", int(result)));
    }

    class Readback
    {
    public:
        explicit Readback(const VulkanDevice& device) : device_(device)
        {
            try
            {
                VkBufferCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                info.size = 384 * 256 * 4;
                info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                check(vkCreateBuffer(device_.handle(), &info, nullptr, &buffer));
                VkMemoryRequirements requirements{};
                vkGetBufferMemoryRequirements(device_.handle(), buffer, &requirements);
                VkMemoryAllocateInfo allocation{};
                allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                allocation.allocationSize = requirements.size;
                allocation.memoryTypeIndex = device_.findMemoryType(
                    requirements.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
                check(vkAllocateMemory(device_.handle(), &allocation, nullptr, &memory_));
                check(vkBindBufferMemory(device_.handle(), buffer, memory_, 0));
            }
            catch (...)
            {
                destroy();
                throw;
            }
        }

        ~Readback()
        {
            destroy();
        }

        Readback(const Readback&) = delete;
        Readback& operator=(const Readback&) = delete;

        std::vector<uint8_t> pixels()
        {
            std::vector<uint8_t> pixels(384 * 256 * 4);
            void* mapped = nullptr;
            check(vkMapMemory(device_.handle(), memory_, 0, pixels.size(), 0, &mapped));
            std::memcpy(pixels.data(), mapped, pixels.size());
            vkUnmapMemory(device_.handle(), memory_);
            return pixels;
        }

        VkBuffer buffer = VK_NULL_HANDLE;

    private:
        void destroy()
        {
            if (buffer)
                vkDestroyBuffer(device_.handle(), buffer, nullptr);
            if (memory_)
                vkFreeMemory(device_.handle(), memory_, nullptr);
        }

        const VulkanDevice& device_;
        VkDeviceMemory memory_ = VK_NULL_HANDLE;
    };

    // Shares attachment/barrier/readback code with model review; model callers supply the
    // existing Renderer pipeline, uploaded buffers and immutable material descriptor sets.
    inline std::vector<uint8_t> captureDraws(const VulkanDevice& device, const VulkanPipeline& pipeline,
                                             VkDescriptorSet frameSet, std::span<const VulkanDrawItem> draws,
                                             VkViewport viewport = {0, 0, 384, 256, 0, 1},
                                             const std::function<void(VkCommandBuffer)>& recordUi = {})
    {
        constexpr VkExtent2D EXTENT{384, 256};
        const auto format = pipeline.colorFormat();
        VulkanImage color(device, EXTENT, format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT);
        VulkanImage depth(device, EXTENT, device.findDepthFormat(), VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                          VK_IMAGE_ASPECT_DEPTH_BIT);
        Readback readback(device);
        device.executeImmediate([&](VkCommandBuffer commands) {
            VkImageMemoryBarrier2 toColor{};
            toColor.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            toColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toColor.srcQueueFamilyIndex = toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toColor.image = color.handle();
            toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            auto toDepth = toColor;
            toDepth.dstStageMask =
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            toDepth.dstAccessMask =
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            toDepth.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            toDepth.image = depth.handle();
            toDepth.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            const std::array barriers = {toColor, toDepth};
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = 2;
            dependency.pImageMemoryBarriers = barriers.data();
            vkCmdPipelineBarrier2(commands, &dependency);

            VkRenderingAttachmentInfo attachment{};
            attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            attachment.imageView = color.view();
            attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            const float gray = 128.0f / 255.0f;
            attachment.clearValue.color = {{gray, gray, gray, 1.0f}};
            auto depthAttachment = attachment;
            depthAttachment.imageView = depth.view();
            depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depthAttachment.clearValue.depthStencil = {1.0f, 0};
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea.extent = EXTENT;
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &attachment;
            rendering.pDepthAttachment = &depthAttachment;
            vkCmdBeginRendering(commands, &rendering);
            const VkRect2D scissor{{0, 0}, EXTENT};
            vkCmdSetViewport(commands, 0, 1, &viewport);
            vkCmdSetScissor(commands, 0, 1, &scissor);
            VulkanCommandList::drawIndexed(commands, pipeline, frameSet, draws);
            if (recordUi)
                recordUi(commands);
            vkCmdEndRendering(commands);

            auto toTransfer = toColor;
            toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            toTransfer.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            toTransfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &toTransfer;
            vkCmdPipelineBarrier2(commands, &dependency);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {EXTENT.width, EXTENT.height, 1};
            vkCmdCopyImageToBuffer(commands, color.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1,
                                   &copy);
            VkBufferMemoryBarrier2 toHost{};
            toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            toHost.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.buffer = readback.buffer;
            toHost.size = VK_WHOLE_SIZE;
            dependency.imageMemoryBarrierCount = 0;
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &toHost;
            vkCmdPipelineBarrier2(commands, &dependency);
        });
        auto pixels = readback.pixels();
        if (format == VK_FORMAT_B8G8R8A8_UNORM)
        {
            for (size_t i = 0; i < pixels.size(); i += 4)
                std::swap(pixels[i], pixels[i + 2]);
        }
        return pixels;
    }

    // Render the application's plane and production shader into the selected UNORM format.
    // The test attachment permits readback without adding a screenshot API to Renderer.
    inline std::vector<uint8_t> capture(VkInstance instance, const VulkanDevice& device, Window::Window& window,
                                        Demo::SceneAssets& assets, VkFormat format,
                                        const std::array<float, 3>& tint = {1.0f, 1.0f, 1.0f},
                                        VkFormat previousPipelineFormat = VK_FORMAT_UNDEFINED)
    {
        auto texture =
            VulkanImage::uploadRgba8(device, {assets.texture->width(), assets.texture->height()},
                                     VK_FORMAT_R8G8B8A8_SRGB, assets.texture->pixels(), VK_IMAGE_USAGE_SAMPLED_BIT);
        VulkanSampler sampler(device);
        struct alignas(16) Transforms
        {
            glm::mat4 view{1.0f};
            glm::mat4 projection{1.0f};
        } transforms;
        transforms.projection[0][0] = 2.0f;
        transforms.projection[1][1] = -2.0f;
        transforms.projection[2][2] = 0.0f;
        const std::array uniforms = {
            std::make_shared<VulkanBuffer>(device, sizeof(transforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        uniforms[0]->write(0, std::as_bytes(std::span(&transforms, 1)));
        VulkanBindingSet bindings(device, uniforms);
        VulkanMaterialBinding material(device, bindings.materialLayout(), *texture, sampler, tint);
        const auto depthFormat = device.findDepthFormat();
        const auto vertexWords = assets.vertex->words();
        const auto fragmentWords = assets.fragment->words();
        auto pipeline = std::make_unique<VulkanPipeline>(
            device, previousPipelineFormat == VK_FORMAT_UNDEFINED ? format : previousPipelineFormat, depthFormat,
            bindings.layout(), bindings.materialLayout(), vertexWords, fragmentWords);
        const auto mesh = Demo::makePlaneMesh();
        auto vertices = VulkanBuffer::uploadStatic(
            device, std::as_bytes(std::span(mesh.vertices)), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
        auto indices =
            VulkanBuffer::uploadStatic(device, std::as_bytes(std::span(mesh.indices)), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                       VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT);
        Integration::ImGuiRenderer ui(window);
        ui.initializeVulkan(instance, device.physicalDevice(), device.handle(), device.queueFamilies().graphics,
                            device.graphicsQueue(), format, depthFormat, 2);
        const auto textureId = assets.texture->id();
        assets.releaseCpuResources();
        if (Resource::ResourceRegistry::instance().getObject(textureId))
            throw Error::Exception(Error::Code::RENDER, "CPU texture remains mounted before GPU readback drawing");
        if (previousPipelineFormat != VK_FORMAT_UNDEFINED)
        {
            // Controlled attachment-format change, reusing the same texture, material and frame bindings.
            // This is not an observed swapchain format change; CPU assets/Loader are already released.
            pipeline = std::make_unique<VulkanPipeline>(device, format, depthFormat, bindings.layout(),
                                                        bindings.materialLayout(), vertexWords, fragmentWords);
        }
        window.pollEvents();
        ui.beginFrame();
        auto* uiDraw = ImGui::GetBackgroundDrawList();
        uiDraw->AddRectFilled(ImVec2(288, 32), ImVec2(368, 96), IM_COL32(255, 0, 255, 255));
        uiDraw->AddRectFilled(ImVec2(296, 40), ImVec2(360, 88), IM_COL32(128, 128, 128, 255));
        uiDraw->AddRectFilled(ImVec2(288, 176), ImVec2(368, 232), IM_COL32(0, 0, 0, 255));
        uiDraw->AddRectFilled(ImVec2(296, 184), ImVec2(360, 224), IM_COL32(128, 128, 128, 128));
        ui.endFrame();

        const std::array draws = {VulkanDrawItem{vertices->handle(), indices->handle(),
                                                 static_cast<uint32_t>(mesh.indices.size()), material.descriptorSet(),
                                                 glm::mat4(1.0f)}};
        return captureDraws(device, *pipeline, bindings.descriptorSet(0), draws, {16, 16, 256, 192, 0, 1},
                            [&](VkCommandBuffer commands) { ui.recordDrawData(ui.drawData(), commands); });
    }
}  // namespace TextureGpuReview
