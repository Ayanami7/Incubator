#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <span>

#include "runtime/render/texture.h"

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
                            VkImageAspectFlags aspect, uint32_t mipLevels = 1, uint32_t arrayLayers = 1,
                            VkImageCreateFlags flags = 0, VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D);
                ~VulkanImage();

                VulkanImage(const VulkanImage&) = delete;
                VulkanImage& operator=(const VulkanImage&) = delete;

                static std::shared_ptr<VulkanImage> uploadRgba8(const VulkanDevice& device, VkExtent2D extent,
                                                                VkFormat format, std::span<const uint8_t> pixels,
                                                                VkImageUsageFlags usage,
                                                                TextureMipMode mipMode = TextureMipMode::BaseLevel);

                /// @brief 上传线性 RGBA16F（2D 或 6 面 Cubemap）多级 mip 纹理。
                static std::shared_ptr<VulkanImage> uploadRgba16f(
                    const VulkanDevice& device, VkExtent2D extent, uint32_t arrayLayers,
                    std::span<const std::span<const uint16_t>> mipPayloads, VkImageUsageFlags usage);

                /// @brief 验证 RGBA8 图像和 mip 链是否受设备支持。
                /// @param extent 基础 mip 的图像尺寸。
                /// @param deviceLimits 设备限制。
                /// @param formatProperties 图像格式属性。
                /// @param imageProperties 对应图像配置的设备属性。
                /// @param mipLevels mip 链级数。
                /// @note 在创建暂存缓冲区或图像前调用；也可用于受控输入。
                static void validateRgba8Support(VkExtent2D extent, const VkPhysicalDeviceLimits& deviceLimits,
                                                 const VkFormatProperties& formatProperties,
                                                 const VkImageFormatProperties& imageProperties,
                                                 uint32_t mipLevels = 1);

                /// @brief 验证 RGBA16F（2D 或 Cubemap）图像和 mip 链是否受设备支持。
                static void validateRgba16fSupport(VkExtent2D extent, uint32_t arrayLayers, uint32_t mipLevels,
                                                   const VkPhysicalDeviceLimits& deviceLimits,
                                                   const VkFormatProperties& formatProperties,
                                                   const VkImageFormatProperties& imageProperties);

                VkImage handle() const noexcept;
                VkImageView view() const noexcept;
                VkExtent2D extent() const noexcept;
                VkFormat format() const noexcept;
                uint32_t mipLevels() const noexcept;
                uint32_t arrayLayers() const noexcept;
                bool isCubemap() const noexcept;

            private:
                const VulkanDevice& device_;
                VkImage image_ = VK_NULL_HANDLE;
                VkImageView view_ = VK_NULL_HANDLE;
                VkDeviceMemory memory_ = VK_NULL_HANDLE;
                VkExtent2D extent_{};
                VkFormat format_ = VK_FORMAT_UNDEFINED;
                uint32_t mipLevels_ = 1;
                uint32_t arrayLayers_ = 1;
                bool isCubemap_ = false;
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
