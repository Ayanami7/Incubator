#include "runtime/render/vulkan/vulkan_image.h"

#include <limits>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/render/vulkan/vulkan_buffer.h"
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
                        throw Error::Exception(Error::Code::RENDER, fmt::format("{} failed with VkResult {}", operation,
                                                                                static_cast<int>(result)));
                    }
                }
            }  // namespace

            VulkanImage::VulkanImage(const VulkanDevice& device, VkExtent2D extent, VkFormat format,
                                     VkImageUsageFlags usage, VkImageAspectFlags aspect)
                : device_(device), extent_(extent), format_(format)
            {
                if (extent_.width == 0 || extent_.height == 0 || format_ == VK_FORMAT_UNDEFINED)
                    throw Error::Exception(Error::Code::RENDER, "Cannot create an empty Vulkan image");
                try
                {
                    VkImageCreateInfo imageInfo{};
                    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                    imageInfo.imageType = VK_IMAGE_TYPE_2D;
                    imageInfo.format = format_;
                    imageInfo.extent = {extent_.width, extent_.height, 1};
                    imageInfo.mipLevels = 1;
                    imageInfo.arrayLayers = 1;
                    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
                    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
                    imageInfo.usage = usage;
                    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    checkVk(vkCreateImage(device_.handle(), &imageInfo, nullptr, &image_), "vkCreateImage");

                    VkMemoryRequirements requirements{};
                    vkGetImageMemoryRequirements(device_.handle(), image_, &requirements);
                    VkMemoryAllocateInfo allocateInfo{};
                    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                    allocateInfo.allocationSize = requirements.size;
                    allocateInfo.memoryTypeIndex =
                        device_.findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                    checkVk(vkAllocateMemory(device_.handle(), &allocateInfo, nullptr, &memory_), "vkAllocateMemory");
                    checkVk(vkBindImageMemory(device_.handle(), image_, memory_, 0), "vkBindImageMemory");

                    VkImageViewCreateInfo viewInfo{};
                    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                    viewInfo.image = image_;
                    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    viewInfo.format = format_;
                    viewInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
                    viewInfo.subresourceRange.aspectMask = aspect;
                    viewInfo.subresourceRange.levelCount = 1;
                    viewInfo.subresourceRange.layerCount = 1;
                    checkVk(vkCreateImageView(device_.handle(), &viewInfo, nullptr, &view_), "vkCreateImageView");
                }
                catch (...)
                {
                    if (view_ != VK_NULL_HANDLE)
                        vkDestroyImageView(device_.handle(), view_, nullptr);
                    if (image_ != VK_NULL_HANDLE)
                        vkDestroyImage(device_.handle(), image_, nullptr);
                    if (memory_ != VK_NULL_HANDLE)
                        vkFreeMemory(device_.handle(), memory_, nullptr);
                    throw;
                }
            }

            VulkanImage::~VulkanImage()
            {
                if (view_ != VK_NULL_HANDLE)
                    vkDestroyImageView(device_.handle(), view_, nullptr);
                if (image_ != VK_NULL_HANDLE)
                    vkDestroyImage(device_.handle(), image_, nullptr);
                if (memory_ != VK_NULL_HANDLE)
                    vkFreeMemory(device_.handle(), memory_, nullptr);
            }

            std::shared_ptr<VulkanImage> VulkanImage::uploadRgba8(const VulkanDevice& device, VkExtent2D extent,
                                                                  VkFormat format, std::span<const uint8_t> pixels,
                                                                  VkImageUsageFlags usage)
            {
                if (extent.width == 0 || extent.height == 0)
                    throw Error::Exception(Error::Code::RENDER, "Cannot upload an empty RGBA8 image");
                if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB)
                    throw Error::Exception(Error::Code::RENDER, "RGBA8 upload requires an R8G8B8A8 image format");
                if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0)
                    throw Error::Exception(Error::Code::RENDER, "RGBA8 upload requires sampled image usage");
                VkFormatProperties formatProperties{};
                vkGetPhysicalDeviceFormatProperties(device.physicalDevice(), format, &formatProperties);
                VkImageFormatProperties imageProperties{};
                checkVk(vkGetPhysicalDeviceImageFormatProperties(
                            device.physicalDevice(), format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                            usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &imageProperties),
                        "vkGetPhysicalDeviceImageFormatProperties(RGBA8 sampled/transfer destination)");
                validateRgba8Support(extent, device.properties().limits, formatProperties, imageProperties);
                const uint64_t pixelCount = static_cast<uint64_t>(extent.width) * extent.height;
                if (pixelCount > std::numeric_limits<size_t>::max() / 4 || pixels.size() != pixelCount * 4)
                {
                    throw Error::Exception(Error::Code::RENDER, "RGBA8 image data size does not match its extent");
                }

                const auto bytes = std::as_bytes(pixels);
                VulkanBuffer staging(device, bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
                staging.write(0, bytes);
                auto image = std::make_shared<VulkanImage>(
                    device, extent, format, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
                device.executeImmediate([&](VkCommandBuffer commandBuffer) {
                    VkImageMemoryBarrier2 toTransfer{};
                    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                    toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                    toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toTransfer.image = image->handle();
                    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    toTransfer.subresourceRange.levelCount = 1;
                    toTransfer.subresourceRange.layerCount = 1;
                    VkDependencyInfo dependency{};
                    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                    dependency.imageMemoryBarrierCount = 1;
                    dependency.pImageMemoryBarriers = &toTransfer;
                    vkCmdPipelineBarrier2(commandBuffer, &dependency);

                    VkBufferImageCopy copy{};
                    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    copy.imageSubresource.layerCount = 1;
                    copy.imageExtent = {extent.width, extent.height, 1};
                    vkCmdCopyBufferToImage(commandBuffer, staging.handle(), image->handle(),
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

                    VkImageMemoryBarrier2 toShaderRead{};
                    toShaderRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                    toShaderRead.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                    toShaderRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    toShaderRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                    toShaderRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toShaderRead.image = image->handle();
                    toShaderRead.subresourceRange = toTransfer.subresourceRange;
                    dependency.pImageMemoryBarriers = &toShaderRead;
                    vkCmdPipelineBarrier2(commandBuffer, &dependency);
                });
                Log::Registry::get("render.vulkan").debug("Uploaded RGBA8 texture: {}x{}, format {}, {} bytes",
                                                          extent.width, extent.height, static_cast<int>(format),
                                                          pixels.size());
                return image;
            }

            void VulkanImage::validateRgba8Support(VkExtent2D extent, const VkPhysicalDeviceLimits& deviceLimits,
                                                   const VkFormatProperties& formatProperties,
                                                   const VkImageFormatProperties& imageProperties)
            {
                constexpr VkFormatFeatureFlags FEATURES =
                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
                if (extent.width == 0 || extent.height == 0 || extent.width > deviceLimits.maxImageDimension2D ||
                    extent.height > deviceLimits.maxImageDimension2D)
                    throw Error::Exception(Error::Code::RENDER,
                                           fmt::format("RGBA8 extent {}x{} exceeds device maxImageDimension2D {}",
                                                       extent.width, extent.height, deviceLimits.maxImageDimension2D));
                if ((formatProperties.optimalTilingFeatures & FEATURES) != FEATURES)
                    throw Error::Exception(Error::Code::RENDER,
                                           "RGBA8 optimal tiling lacks sampled/transfer-dst support");
                const uint64_t pixels = static_cast<uint64_t>(extent.width) * extent.height;
                if (extent.width > imageProperties.maxExtent.width ||
                    extent.height > imageProperties.maxExtent.height || imageProperties.maxExtent.depth < 1 ||
                    imageProperties.maxMipLevels < 1 || imageProperties.maxArrayLayers < 1 ||
                    (imageProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT) == 0 ||
                    pixels > imageProperties.maxResourceSize / 4u)
                    throw Error::Exception(Error::Code::RENDER,
                                           "RGBA8 image format limits do not support this single-mip 2D texture");
            }

            VkImage VulkanImage::handle() const noexcept
            {
                return image_;
            }

            VkImageView VulkanImage::view() const noexcept
            {
                return view_;
            }

            VkExtent2D VulkanImage::extent() const noexcept
            {
                return extent_;
            }

            VkFormat VulkanImage::format() const noexcept
            {
                return format_;
            }
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
