#include "runtime/render/vulkan/vulkan_frame_scheduler.h"

#include <array>
#include <limits>

#include "runtime/core/error/error.h"
#include "runtime/render/vulkan/vulkan_device.h"
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

            VulkanFrameScheduler::VulkanFrameScheduler(const VulkanDevice& device) : device_(device)
            {
                try
                {
                    for (FrameSlot& frame : frames_)
                    {
                        VkCommandPoolCreateInfo poolInfo{};
                        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                        poolInfo.queueFamilyIndex = device_.queueFamilies().graphics;
                        checkVk(vkCreateCommandPool(device_.handle(), &poolInfo, nullptr, &frame.commandPool),
                                "vkCreateCommandPool");

                        VkCommandBufferAllocateInfo allocateInfo{};
                        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                        allocateInfo.commandPool = frame.commandPool;
                        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                        allocateInfo.commandBufferCount = 1;
                        checkVk(vkAllocateCommandBuffers(device_.handle(), &allocateInfo, &frame.commandBuffer),
                                "vkAllocateCommandBuffers");

                        VkSemaphoreCreateInfo semaphoreInfo{};
                        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                        checkVk(vkCreateSemaphore(device_.handle(), &semaphoreInfo, nullptr, &frame.imageAvailable),
                                "vkCreateSemaphore");

                        VkFenceCreateInfo fenceInfo{};
                        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
                        checkVk(vkCreateFence(device_.handle(), &fenceInfo, nullptr, &frame.completionFence),
                                "vkCreateFence");
                    }
                }
                catch (...)
                {
                    for (FrameSlot& frame : frames_)
                    {
                        if (frame.completionFence != VK_NULL_HANDLE)
                            vkDestroyFence(device_.handle(), frame.completionFence, nullptr);
                        if (frame.imageAvailable != VK_NULL_HANDLE)
                            vkDestroySemaphore(device_.handle(), frame.imageAvailable, nullptr);
                        if (frame.commandPool != VK_NULL_HANDLE)
                            vkDestroyCommandPool(device_.handle(), frame.commandPool, nullptr);
                    }
                    throw;
                }
            }

            VulkanFrameScheduler::~VulkanFrameScheduler()
            {
                for (size_t index = 0; index < presentFences_.size(); ++index)
                {
                    if (presentPending_[index])
                    {
                        vkWaitForFences(device_.handle(), 1, &presentFences_[index], VK_TRUE,
                                        std::numeric_limits<uint64_t>::max());
                    }
                }
                vkDeviceWaitIdle(device_.handle());
                destroySwapchainSync();
                for (FrameSlot& frame : frames_)
                {
                    if (frame.completionFence != VK_NULL_HANDLE)
                        vkDestroyFence(device_.handle(), frame.completionFence, nullptr);
                    if (frame.imageAvailable != VK_NULL_HANDLE)
                        vkDestroySemaphore(device_.handle(), frame.imageAvailable, nullptr);
                    if (frame.commandPool != VK_NULL_HANDLE)
                        vkDestroyCommandPool(device_.handle(), frame.commandPool, nullptr);
                }
            }

            void VulkanFrameScheduler::initializeSwapchainImages(uint32_t imageCount)
            {
                destroySwapchainSync();
                presentReady_.resize(imageCount, VK_NULL_HANDLE);
                presentFences_.resize(imageCount, VK_NULL_HANDLE);
                presentPending_.assign(imageCount, false);
                imageOwners_.assign(imageCount, VK_NULL_HANDLE);
                VkSemaphoreCreateInfo semaphoreInfo{};
                semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                VkFenceCreateInfo fenceInfo{};
                fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                try
                {
                    for (uint32_t index = 0; index < imageCount; ++index)
                    {
                        checkVk(vkCreateSemaphore(device_.handle(), &semaphoreInfo, nullptr, &presentReady_[index]),
                                "vkCreateSemaphore");
                        checkVk(vkCreateFence(device_.handle(), &fenceInfo, nullptr, &presentFences_[index]),
                                "vkCreateFence");
                    }
                }
                catch (...)
                {
                    destroySwapchainSync();
                    throw;
                }
            }

            void VulkanFrameScheduler::waitForFrames()
            {
                std::array<VkFence, 2> fences{};
                for (size_t index = 0; index < frames_.size(); ++index)
                    fences[index] = frames_[index].completionFence;
                checkVk(vkWaitForFences(device_.handle(), static_cast<uint32_t>(frames_.size()), fences.data(), VK_TRUE,
                                        std::numeric_limits<uint64_t>::max()),
                        "vkWaitForFences");
            }

            void VulkanFrameScheduler::waitForPresentation()
            {
                for (size_t index = 0; index < presentFences_.size(); ++index)
                {
                    if (presentPending_[index])
                    {
                        checkVk(vkWaitForFences(device_.handle(), 1, &presentFences_[index], VK_TRUE,
                                                std::numeric_limits<uint64_t>::max()),
                                "vkWaitForFences(present)");
                    }
                }
            }

            bool VulkanFrameScheduler::renderAndPresent(const VulkanSwapchain& swapchain,
                                                        const VkClearColorValue& clearColor)
            {
                FrameSlot& frame = frames_[frameIndex_];
                checkVk(vkWaitForFences(device_.handle(), 1, &frame.completionFence, VK_TRUE,
                                        std::numeric_limits<uint64_t>::max()),
                        "vkWaitForFences(frame)");

                uint32_t imageIndex = 0;
                const VkResult acquireResult = vkAcquireNextImageKHR(
                    device_.handle(), swapchain.handle(), std::numeric_limits<uint64_t>::max(), frame.imageAvailable,
                    VK_NULL_HANDLE, &imageIndex);
                if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
                    return false;
                if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
                    checkVk(acquireResult, "vkAcquireNextImageKHR");
                bool shouldRecreate = acquireResult == VK_SUBOPTIMAL_KHR;

                if (imageOwners_[imageIndex] != VK_NULL_HANDLE && imageOwners_[imageIndex] != frame.completionFence)
                {
                    checkVk(vkWaitForFences(device_.handle(), 1, &imageOwners_[imageIndex], VK_TRUE,
                                            std::numeric_limits<uint64_t>::max()),
                            "vkWaitForFences(image owner)");
                }
                if (presentPending_[imageIndex])
                {
                    checkVk(vkWaitForFences(device_.handle(), 1, &presentFences_[imageIndex], VK_TRUE,
                                            std::numeric_limits<uint64_t>::max()),
                            "vkWaitForFences(present image)");
                    checkVk(vkResetFences(device_.handle(), 1, &presentFences_[imageIndex]),
                            "vkResetFences(present image)");
                    presentPending_[imageIndex] = false;
                }

                checkVk(vkResetCommandPool(device_.handle(), frame.commandPool, 0), "vkResetCommandPool");
                recordClear(swapchain, imageIndex, clearColor, frame.commandBuffer);

                VkSemaphoreSubmitInfo waitInfo{};
                waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
                waitInfo.semaphore = frame.imageAvailable;
                waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                VkCommandBufferSubmitInfo commandInfo{};
                commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
                commandInfo.commandBuffer = frame.commandBuffer;
                VkSemaphoreSubmitInfo signalInfo{};
                signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
                signalInfo.semaphore = presentReady_[imageIndex];
                signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                VkSubmitInfo2 submitInfo{};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
                submitInfo.waitSemaphoreInfoCount = 1;
                submitInfo.pWaitSemaphoreInfos = &waitInfo;
                submitInfo.commandBufferInfoCount = 1;
                submitInfo.pCommandBufferInfos = &commandInfo;
                submitInfo.signalSemaphoreInfoCount = 1;
                submitInfo.pSignalSemaphoreInfos = &signalInfo;

                // Reset only after acquisition and command recording have succeeded.
                checkVk(vkResetFences(device_.handle(), 1, &frame.completionFence), "vkResetFences(frame)");
                checkVk(vkQueueSubmit2(device_.graphicsQueue(), 1, &submitInfo, frame.completionFence),
                        "vkQueueSubmit2");
                imageOwners_[imageIndex] = frame.completionFence;

                VkSwapchainPresentFenceInfoEXT presentFenceInfo{};
                presentFenceInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
                presentFenceInfo.swapchainCount = 1;
                presentFenceInfo.pFences = &presentFences_[imageIndex];
                VkPresentInfoKHR presentInfo{};
                presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
                presentInfo.pNext = &presentFenceInfo;
                presentInfo.waitSemaphoreCount = 1;
                presentInfo.pWaitSemaphores = &presentReady_[imageIndex];
                const VkSwapchainKHR swapchainHandle = swapchain.handle();
                presentInfo.swapchainCount = 1;
                presentInfo.pSwapchains = &swapchainHandle;
                presentInfo.pImageIndices = &imageIndex;
                const VkResult presentResult = vkQueuePresentKHR(device_.presentQueue(), &presentInfo);
                if (presentResult == VK_SUCCESS || presentResult == VK_SUBOPTIMAL_KHR)
                {
                    presentPending_[imageIndex] = true;
                    shouldRecreate = shouldRecreate || presentResult == VK_SUBOPTIMAL_KHR;
                }
                else if (presentResult == VK_ERROR_OUT_OF_DATE_KHR)
                {
                    // The queue wait and present fence are still submitted for this rejected request.
                    presentPending_[imageIndex] = true;
                    shouldRecreate = true;
                }
                else if (presentResult == VK_ERROR_SURFACE_LOST_KHR)
                {
                    // The queue wait is still submitted even though the presentation engine rejected the surface.
                    presentPending_[imageIndex] = true;
                    checkVk(presentResult, "vkQueuePresentKHR");
                }
                else
                {
                    checkVk(presentResult, "vkQueuePresentKHR");
                }

                frameIndex_ = (frameIndex_ + 1) % static_cast<uint32_t>(frames_.size());
                return !shouldRecreate;
            }

            void VulkanFrameScheduler::destroySwapchainSync() noexcept
            {
                for (VkSemaphore semaphore : presentReady_)
                {
                    if (semaphore != VK_NULL_HANDLE)
                        vkDestroySemaphore(device_.handle(), semaphore, nullptr);
                }
                for (VkFence fence : presentFences_)
                {
                    if (fence != VK_NULL_HANDLE)
                        vkDestroyFence(device_.handle(), fence, nullptr);
                }
                presentReady_.clear();
                presentFences_.clear();
                presentPending_.clear();
                imageOwners_.clear();
            }

            void VulkanFrameScheduler::recordClear(const VulkanSwapchain& swapchain, uint32_t imageIndex,
                                                    const VkClearColorValue& clearColor,
                                                    VkCommandBuffer commandBuffer)
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
                VkDependencyInfo dependency{};
                dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dependency.imageMemoryBarrierCount = 1;
                dependency.pImageMemoryBarriers = &toColor;
                vkCmdPipelineBarrier2(commandBuffer, &dependency);

                VkRenderingAttachmentInfo attachment{};
                attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                attachment.imageView = swapchain.imageView(imageIndex);
                attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                attachment.clearValue.color = clearColor;
                VkRenderingInfo rendering{};
                rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                rendering.renderArea.extent = swapchain.extent();
                rendering.layerCount = 1;
                rendering.colorAttachmentCount = 1;
                rendering.pColorAttachments = &attachment;
                vkCmdBeginRendering(commandBuffer, &rendering);
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
                vkCmdPipelineBarrier2(commandBuffer, &dependency);
                checkVk(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");
            }
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
