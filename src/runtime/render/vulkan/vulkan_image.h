#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <span>

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            class VulkanDevice;

            class VulkanImage
            {
            public:
                VulkanImage(const VulkanDevice& device, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
                            VkImageAspectFlags aspect);
                ~VulkanImage();

                VulkanImage(const VulkanImage&) = delete;
                VulkanImage& operator=(const VulkanImage&) = delete;

                static std::shared_ptr<VulkanImage> uploadRgba8(const VulkanDevice& device, VkExtent2D extent,
                                                                VkFormat format, std::span<const uint8_t> pixels,
                                                                VkImageUsageFlags usage);

                // Checks queried limits before staging or image allocation; also usable with controlled inputs.
                static void validateRgba8Support(VkExtent2D extent, const VkPhysicalDeviceLimits& deviceLimits,
                                                 const VkFormatProperties& formatProperties,
                                                 const VkImageFormatProperties& imageProperties);

                VkImage handle() const noexcept;
                VkImageView view() const noexcept;
                VkExtent2D extent() const noexcept;
                VkFormat format() const noexcept;

            private:
                const VulkanDevice& device_;
                VkImage image_ = VK_NULL_HANDLE;
                VkImageView view_ = VK_NULL_HANDLE;
                VkDeviceMemory memory_ = VK_NULL_HANDLE;
                VkExtent2D extent_{};
                VkFormat format_ = VK_FORMAT_UNDEFINED;
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
