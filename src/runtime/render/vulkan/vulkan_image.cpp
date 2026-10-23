#include "runtime/render/vulkan/vulkan_image.h"

#include <algorithm>
#include <bit>
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
                                     VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t mipLevels)
                : device_(device), extent_(extent), format_(format), mipLevels_(mipLevels)
            {
                if (extent_.width == 0 || extent_.height == 0 || format_ == VK_FORMAT_UNDEFINED || mipLevels_ == 0 ||
                    mipLevels_ > std::bit_width(std::max(extent_.width, extent_.height)))
                    throw Error::Exception(Error::Code::RENDER, "Cannot create an empty Vulkan image");
                try
                {
                    VkImageCreateInfo imageInfo{};
                    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                    imageInfo.imageType = VK_IMAGE_TYPE_2D;
                    imageInfo.format = format_;
                    imageInfo.extent = {extent_.width, extent_.height, 1};
                    imageInfo.mipLevels = mipLevels_;
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
                    viewInfo.subresourceRange.levelCount = mipLevels_;
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
                                                                  VkImageUsageFlags usage, TextureMipMode mipMode)
            {
                if (extent.width == 0 || extent.height == 0)
                    throw Error::Exception(Error::Code::RENDER, "Cannot upload an empty RGBA8 image");
                if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB)
                    throw Error::Exception(Error::Code::RENDER, "RGBA8 upload requires an R8G8B8A8 image format");
                if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0)
                    throw Error::Exception(Error::Code::RENDER, "RGBA8 upload requires sampled image usage");
                if (mipMode != TextureMipMode::BaseLevel && mipMode != TextureMipMode::FullChain)
                    throw Error::Exception(Error::Code::RENDER, "Invalid texture mip mode");
                const uint32_t mipLevels =
                    mipMode == TextureMipMode::FullChain ? std::bit_width(std::max(extent.width, extent.height)) : 1;
                usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                if (mipLevels > 1)
                    usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                VkFormatProperties formatProperties{};
                vkGetPhysicalDeviceFormatProperties(device.physicalDevice(), format, &formatProperties);
                VkImageFormatProperties imageProperties{};
                checkVk(vkGetPhysicalDeviceImageFormatProperties(device.physicalDevice(), format, VK_IMAGE_TYPE_2D,
                                                                 VK_IMAGE_TILING_OPTIMAL, usage, 0, &imageProperties),
                        "vkGetPhysicalDeviceImageFormatProperties(RGBA8 upload usage)");
                validateRgba8Support(extent, device.properties().limits, formatProperties, imageProperties, mipLevels);
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
                auto image =
                    std::make_shared<VulkanImage>(device, extent, format, usage, VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);
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
                    toTransfer.subresourceRange.levelCount = mipLevels;
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
                    toShaderRead.subresourceRange.levelCount = 1;
                    VkExtent2D sourceExtent = extent;
                    for (uint32_t level = 1; level < mipLevels; ++level)
                    {
                        auto toSource = toShaderRead;
                        toSource.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                        toSource.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                        toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        toSource.subresourceRange.baseMipLevel = level - 1;
                        dependency.pImageMemoryBarriers = &toSource;
                        vkCmdPipelineBarrier2(commandBuffer, &dependency);

                        const VkExtent2D destinationExtent{std::max(1u, sourceExtent.width / 2),
                                                           std::max(1u, sourceExtent.height / 2)};
                        VkImageBlit blit{};
                        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
                        blit.srcOffsets[1] = {static_cast<int32_t>(sourceExtent.width),
                                              static_cast<int32_t>(sourceExtent.height), 1};
                        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                        blit.dstOffsets[1] = {static_cast<int32_t>(destinationExtent.width),
                                              static_cast<int32_t>(destinationExtent.height), 1};
                        vkCmdBlitImage(commandBuffer, image->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       image->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                                       VK_FILTER_LINEAR);

                        auto sourceToShader = toShaderRead;
                        sourceToShader.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                        sourceToShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        sourceToShader.subresourceRange.baseMipLevel = level - 1;
                        dependency.pImageMemoryBarriers = &sourceToShader;
                        vkCmdPipelineBarrier2(commandBuffer, &dependency);
                        sourceExtent = destinationExtent;
                    }
                    toShaderRead.subresourceRange.baseMipLevel = mipLevels - 1;
                    dependency.pImageMemoryBarriers = &toShaderRead;
                    vkCmdPipelineBarrier2(commandBuffer, &dependency);
                });
                Log::Registry::get("render.vulkan")
                    .debug("Uploaded RGBA8 texture: {}x{}, format {}, {} bytes, {} mips", extent.width, extent.height,
                           static_cast<int>(format), pixels.size(), mipLevels);
                return image;
            }

            void VulkanImage::validateRgba8Support(VkExtent2D extent, const VkPhysicalDeviceLimits& deviceLimits,
                                                   const VkFormatProperties& formatProperties,
                                                   const VkImageFormatProperties& imageProperties, uint32_t mipLevels)
            {
                VkFormatFeatureFlags features =
                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
                if (mipLevels > 1)
                    features |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
                if (extent.width == 0 || extent.height == 0 || extent.width > deviceLimits.maxImageDimension2D ||
                    extent.height > deviceLimits.maxImageDimension2D)
                    throw Error::Exception(Error::Code::RENDER,
                                           fmt::format("RGBA8 extent {}x{} exceeds device maxImageDimension2D {}",
                                                       extent.width, extent.height, deviceLimits.maxImageDimension2D));
                if ((formatProperties.optimalTilingFeatures & features) != features)
                    throw Error::Exception(Error::Code::RENDER,
                                           "RGBA8 optimal tiling lacks required upload/filter/blit support");
                if (extent.width > imageProperties.maxExtent.width ||
                    extent.height > imageProperties.maxExtent.height || imageProperties.maxExtent.depth < 1 ||
                    mipLevels == 0 || mipLevels > std::bit_width(std::max(extent.width, extent.height)) ||
                    imageProperties.maxMipLevels < mipLevels || imageProperties.maxArrayLayers < 1 ||
                    (imageProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT) == 0)
                    throw Error::Exception(Error::Code::RENDER,
                                           "RGBA8 image format limits do not support this 2D texture mip count");
                uint64_t remainingPixels = imageProperties.maxResourceSize / 4u;
                for (uint32_t level = 0; level < mipLevels; ++level)
                {
                    const uint64_t pixels = static_cast<uint64_t>(extent.width) * extent.height;
                    if (pixels > remainingPixels)
                        throw Error::Exception(Error::Code::RENDER, "RGBA8 mip chain exceeds image maxResourceSize");
                    remainingPixels -= pixels;
                    extent = {std::max(1u, extent.width / 2), std::max(1u, extent.height / 2)};
                }
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

            uint32_t VulkanImage::mipLevels() const noexcept
            {
                return mipLevels_;
            }
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
