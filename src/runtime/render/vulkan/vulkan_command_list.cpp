#include "runtime/render/vulkan/vulkan_command_list.h"

#include "runtime/core/error/error.h"
#include "runtime/render/vulkan/vulkan_buffer.h"
#include "runtime/render/vulkan/vulkan_pipeline.h"
#include "runtime/render/vulkan/vulkan_swapchain.h"

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            namespace
            {
                void checkVk(VkResult result, const char* operation)
                {
                    if (result != VK_SUCCESS)
                    {
                        throw Error::Exception(Error::Code::RENDER,
                                               fmt::format("{} failed with VkResult {}", operation,
                                                           static_cast<int>(result)));
                    }
                }
            }  // namespace

            void VulkanCommandList::recordIndexed(VkCommandBuffer commandBuffer, const VulkanSwapchain& swapchain,
                                                  uint32_t imageIndex, const VkClearColorValue& clearColor,
                                                  const VulkanPipeline& pipeline,
                                                  const VulkanBuffer& vertexBuffer, const VulkanBuffer& indexBuffer,
                                                  VkImage depthImage, VkImageView depthView,
                                                  VkDescriptorSet descriptorSet, uint32_t indexCount,
                                                  const std::function<void(VkCommandBuffer)>& recordUi)
            {
                VkCommandBufferBeginInfo beginInfo{};
                beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer");

                VkImageMemoryBarrier2 toColor{};
                toColor.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                toColor.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                toColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toColor.image = swapchain.image(imageIndex);
                toColor.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                toColor.subresourceRange.levelCount = 1;
                toColor.subresourceRange.layerCount = 1;
                VkImageMemoryBarrier2 toDepth{};
                toDepth.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                toDepth.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                       VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                toDepth.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                toDepth.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                toDepth.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                toDepth.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toDepth.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toDepth.image = depthImage;
                toDepth.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                toDepth.subresourceRange.levelCount = 1;
                toDepth.subresourceRange.layerCount = 1;
                VkDependencyInfo dependency{};
                dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                const VkImageMemoryBarrier2 initialBarriers[] = {toColor, toDepth};
                dependency.imageMemoryBarrierCount = 2;
                dependency.pImageMemoryBarriers = initialBarriers;
                vkCmdPipelineBarrier2(commandBuffer, &dependency);

                VkRenderingAttachmentInfo attachment{};
                attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                attachment.imageView = swapchain.imageView(imageIndex);
                attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                attachment.clearValue.color = clearColor;
                VkRenderingAttachmentInfo depthAttachment{};
                depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                depthAttachment.imageView = depthView;
                depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                depthAttachment.clearValue.depthStencil = {1.0f, 0};
                VkRenderingInfo rendering{};
                rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                rendering.renderArea.extent = swapchain.extent();
                rendering.layerCount = 1;
                rendering.colorAttachmentCount = 1;
                rendering.pColorAttachments = &attachment;
                rendering.pDepthAttachment = &depthAttachment;
                vkCmdBeginRendering(commandBuffer, &rendering);

                const VkExtent2D extent = swapchain.extent();
                VkViewport viewport{};
                viewport.width = static_cast<float>(extent.width);
                viewport.height = static_cast<float>(extent.height);
                viewport.maxDepth = 1.0f;
                VkRect2D scissor{};
                scissor.extent = extent;
                vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
                vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0, 1,
                                        &descriptorSet, 0, nullptr);
                const VkDeviceSize vertexOffset = 0;
                const VkBuffer vertexHandle = vertexBuffer.handle();
                vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexHandle, &vertexOffset);
                vkCmdBindIndexBuffer(commandBuffer, indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT16);
                vkCmdDrawIndexed(commandBuffer, indexCount, 1, 0, 0, 0);
                if (recordUi)
                    recordUi(commandBuffer);
                vkCmdEndRendering(commandBuffer);

                VkImageMemoryBarrier2 toPresent{};
                toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                toPresent.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                toPresent.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                toPresent.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
                toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toPresent.image = swapchain.image(imageIndex);
                toPresent.subresourceRange = toColor.subresourceRange;
                dependency.pImageMemoryBarriers = &toPresent;
                dependency.imageMemoryBarrierCount = 1;
                vkCmdPipelineBarrier2(commandBuffer, &dependency);
                checkVk(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");
            }
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
