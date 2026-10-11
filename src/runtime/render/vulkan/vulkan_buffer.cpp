#include "runtime/render/vulkan/vulkan_buffer.h"

#include <cstring>
#include <limits>

#include "runtime/core/error/error.h"
#include "runtime/render/vulkan/vulkan_device.h"

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

            VulkanBuffer::VulkanBuffer(const VulkanDevice& device, VkDeviceSize size, VkBufferUsageFlags usage,
                                       VkMemoryPropertyFlags requiredMemory,
                                       VkMemoryPropertyFlags preferredMemory)
                : device_(device), size_(size)
            {
                if (size_ == 0)
                    throw Error::Exception(Error::Code::RENDER, "Cannot create an empty Vulkan buffer");

                try
                {
                    VkBufferCreateInfo bufferInfo{};
                    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                    bufferInfo.size = size_;
                    bufferInfo.usage = usage;
                    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                    checkVk(vkCreateBuffer(device_.handle(), &bufferInfo, nullptr, &buffer_), "vkCreateBuffer");

                    VkMemoryRequirements requirements{};
                    vkGetBufferMemoryRequirements(device_.handle(), buffer_, &requirements);
                    allocationSize_ = requirements.size;
                    const uint32_t memoryType = device_.findMemoryType(requirements.memoryTypeBits, requiredMemory,
                                                                       preferredMemory, &memoryProperties_);
                    VkMemoryAllocateInfo allocateInfo{};
                    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                    allocateInfo.allocationSize = allocationSize_;
                    allocateInfo.memoryTypeIndex = memoryType;
                    checkVk(vkAllocateMemory(device_.handle(), &allocateInfo, nullptr, &memory_), "vkAllocateMemory");
                    checkVk(vkBindBufferMemory(device_.handle(), buffer_, memory_, 0), "vkBindBufferMemory");
                }
                catch (...)
                {
                    if (buffer_ != VK_NULL_HANDLE)
                        vkDestroyBuffer(device_.handle(), buffer_, nullptr);
                    if (memory_ != VK_NULL_HANDLE)
                        vkFreeMemory(device_.handle(), memory_, nullptr);
                    throw;
                }
            }

            VulkanBuffer::~VulkanBuffer()
            {
                if (buffer_ != VK_NULL_HANDLE)
                    vkDestroyBuffer(device_.handle(), buffer_, nullptr);
                if (memory_ != VK_NULL_HANDLE)
                    vkFreeMemory(device_.handle(), memory_, nullptr);
            }

            std::shared_ptr<VulkanBuffer> VulkanBuffer::uploadStatic(const VulkanDevice& device,
                                                                     std::span<const std::byte> data,
                                                                     VkBufferUsageFlags usage,
                                                                     VkPipelineStageFlags2 consumerStage,
                                                                     VkAccessFlags2 consumerAccess)
            {
                if (data.empty())
                    throw Error::Exception(Error::Code::RENDER, "Cannot upload empty Vulkan buffer data");

                VulkanBuffer staging(device, data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
                staging.write(0, data);
                auto destination = std::make_shared<VulkanBuffer>(device, data.size(),
                                                                  usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                device.executeImmediate([&](VkCommandBuffer commandBuffer) {
                    VkBufferCopy copy{};
                    copy.size = data.size();
                    vkCmdCopyBuffer(commandBuffer, staging.handle(), destination->handle(), 1, &copy);

                    VkBufferMemoryBarrier2 toConsumer{};
                    toConsumer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                    toConsumer.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                    toConsumer.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    toConsumer.dstStageMask = consumerStage;
                    toConsumer.dstAccessMask = consumerAccess;
                    toConsumer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toConsumer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toConsumer.buffer = destination->handle();
                    toConsumer.size = VK_WHOLE_SIZE;
                    VkDependencyInfo dependency{};
                    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                    dependency.bufferMemoryBarrierCount = 1;
                    dependency.pBufferMemoryBarriers = &toConsumer;
                    vkCmdPipelineBarrier2(commandBuffer, &dependency);
                });
                return destination;
            }

            void VulkanBuffer::write(VkDeviceSize offset, std::span<const std::byte> data)
            {
                if ((memoryProperties_ & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
                {
                    throw Error::Exception(Error::Code::RENDER, "Cannot write a non-host-visible Vulkan buffer");
                }
                if (data.empty() || offset > size_ || data.size() > size_ - offset)
                {
                    throw Error::Exception(Error::Code::RENDER, "Vulkan buffer write is outside the buffer bounds");
                }

                void* mapped = nullptr;
                checkVk(vkMapMemory(device_.handle(), memory_, 0, allocationSize_, 0, &mapped), "vkMapMemory");
                std::memcpy(static_cast<std::byte*>(mapped) + offset, data.data(), data.size());
                if ((memoryProperties_ & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
                {
                    VkMappedMemoryRange range{};
                    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                    range.memory = memory_;
                    range.offset = 0;
                    range.size = VK_WHOLE_SIZE;
                    const VkResult flushResult = vkFlushMappedMemoryRanges(device_.handle(), 1, &range);
                    vkUnmapMemory(device_.handle(), memory_);
                    checkVk(flushResult, "vkFlushMappedMemoryRanges");
                    return;
                }
                vkUnmapMemory(device_.handle(), memory_);
            }

            VkBuffer VulkanBuffer::handle() const noexcept
            {
                return buffer_;
            }

            VkDeviceSize VulkanBuffer::size() const noexcept
            {
                return size_;
            }
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
