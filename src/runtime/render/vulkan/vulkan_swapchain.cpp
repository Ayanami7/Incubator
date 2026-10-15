#include "runtime/render/vulkan/vulkan_swapchain.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/render/vulkan/glfw_surface.h"
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

                VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities, int width, int height)
                {
                    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
                    {
                        return capabilities.currentExtent;
                    }
                    return {std::clamp(static_cast<uint32_t>(width), capabilities.minImageExtent.width,
                                       capabilities.maxImageExtent.width),
                            std::clamp(static_cast<uint32_t>(height), capabilities.minImageExtent.height,
                                       capabilities.maxImageExtent.height)};
                }
            }  // namespace

            VulkanSwapchain::VulkanSwapchain(const VulkanDevice& device, const GlfwSurface& surface, int width,
                                             int height)
                : device_(device), surface_(surface)
            {
                recreate(width, height);
            }

            VulkanSwapchain::~VulkanSwapchain()
            {
                destroy();
            }

            VkSurfaceFormatKHR VulkanSwapchain::chooseSurfaceFormat(std::span<const VkSurfaceFormatKHR> formats)
            {
                constexpr VkColorSpaceKHR COLOR_SPACE = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED &&
                    formats[0].colorSpace == COLOR_SPACE)
                    return {VK_FORMAT_B8G8R8A8_UNORM, COLOR_SPACE};
                for (const auto preferred : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM})
                {
                    for (const auto& candidate : formats)
                    {
                        if (candidate.format == preferred && candidate.colorSpace == COLOR_SPACE)
                            return candidate;
                    }
                }
                std::string candidates;
                for (const auto& candidate : formats)
                    candidates += fmt::format(" (format {}, colorSpace {})", static_cast<int>(candidate.format),
                                              static_cast<int>(candidate.colorSpace));
                throw Error::Exception(Error::Code::RENDER,
                                       "No supported UNORM/SRGB_NONLINEAR surface format; candidates:" + candidates);
            }

            bool VulkanSwapchain::recreate(int width, int height)
            {
                VkSurfaceCapabilitiesKHR capabilities{};
                checkVk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_.physicalDevice(), surface_.handle(),
                                                                  &capabilities),
                        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
                if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0)
                {
                    throw Error::Exception(Error::Code::RENDER,
                                           "The surface does not support color attachment swapchain images");
                }

                uint32_t formatCount = 0;
                checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(device_.physicalDevice(), surface_.handle(), &formatCount,
                                                             nullptr),
                        "vkGetPhysicalDeviceSurfaceFormatsKHR");
                if (formatCount == 0)
                {
                    throw Error::Exception(Error::Code::RENDER, "The surface has no supported formats");
                }
                std::vector<VkSurfaceFormatKHR> formats(formatCount);
                checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(device_.physicalDevice(), surface_.handle(), &formatCount,
                                                             formats.data()),
                        "vkGetPhysicalDeviceSurfaceFormatsKHR");
                formats.resize(formatCount);
                const VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(formats);
                VkFormatProperties formatProperties{};
                vkGetPhysicalDeviceFormatProperties(device_.physicalDevice(), surfaceFormat.format, &formatProperties);
                if ((formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0)
                    throw Error::Exception(Error::Code::RENDER, "Selected surface format cannot be a color attachment");
                const VkExtent2D newExtent = chooseExtent(capabilities, width, height);
                if (newExtent.width == 0 || newExtent.height == 0)
                {
                    return false;
                }

                uint32_t imageCount = std::max(capabilities.minImageCount, 2u);
                if (capabilities.maxImageCount != 0)
                {
                    imageCount = std::min(imageCount, capabilities.maxImageCount);
                }

                VkSwapchainCreateInfoKHR createInfo{};
                createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
                createInfo.surface = surface_.handle();
                createInfo.minImageCount = imageCount;
                createInfo.imageFormat = surfaceFormat.format;
                createInfo.imageColorSpace = surfaceFormat.colorSpace;
                createInfo.imageExtent = newExtent;
                createInfo.imageArrayLayers = 1;
                createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                const QueueFamilyIndices& queues = device_.queueFamilies();
                const uint32_t familyIndices[] = {queues.graphics, queues.present};
                if (queues.graphics != queues.present)
                {
                    createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
                    createInfo.queueFamilyIndexCount = 2;
                    createInfo.pQueueFamilyIndices = familyIndices;
                }
                else
                {
                    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
                }
                createInfo.preTransform = capabilities.currentTransform;
                constexpr VkCompositeAlphaFlagBitsKHR compositeAlphaModes[] = {
                    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                    VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
                createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
                for (VkCompositeAlphaFlagBitsKHR mode : compositeAlphaModes)
                {
                    if ((capabilities.supportedCompositeAlpha & mode) != 0)
                    {
                        createInfo.compositeAlpha = mode;
                        break;
                    }
                }
                createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
                createInfo.clipped = VK_TRUE;
                createInfo.oldSwapchain = swapchain_;

                VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
                checkVk(vkCreateSwapchainKHR(device_.handle(), &createInfo, nullptr, &newSwapchain),
                        "vkCreateSwapchainKHR");
                std::vector<VkImage> newImages;
                std::vector<VkImageView> newViews;
                try
                {
                    uint32_t newImageCount = 0;
                    checkVk(vkGetSwapchainImagesKHR(device_.handle(), newSwapchain, &newImageCount, nullptr),
                            "vkGetSwapchainImagesKHR");
                    newImages.resize(newImageCount);
                    checkVk(vkGetSwapchainImagesKHR(device_.handle(), newSwapchain, &newImageCount, newImages.data()),
                            "vkGetSwapchainImagesKHR");
                    newImages.resize(newImageCount);
                    newViews.reserve(newImageCount);
                    for (VkImage image : newImages)
                    {
                        VkImageViewCreateInfo viewInfo{};
                        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                        viewInfo.image = image;
                        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                        viewInfo.format = surfaceFormat.format;
                        viewInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
                        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                        viewInfo.subresourceRange.levelCount = 1;
                        viewInfo.subresourceRange.layerCount = 1;
                        VkImageView view = VK_NULL_HANDLE;
                        checkVk(vkCreateImageView(device_.handle(), &viewInfo, nullptr, &view), "vkCreateImageView");
                        newViews.push_back(view);
                    }
                }
                catch (...)
                {
                    for (VkImageView view : newViews)
                    {
                        vkDestroyImageView(device_.handle(), view, nullptr);
                    }
                    vkDestroySwapchainKHR(device_.handle(), newSwapchain, nullptr);
                    throw;
                }

                destroy();
                swapchain_ = newSwapchain;
                format_ = surfaceFormat.format;
                colorSpace_ = surfaceFormat.colorSpace;
                extent_ = newExtent;
                images_ = std::move(newImages);
                imageViews_ = std::move(newViews);
                return true;
            }

            void VulkanSwapchain::destroy() noexcept
            {
                for (VkImageView view : imageViews_)
                {
                    vkDestroyImageView(device_.handle(), view, nullptr);
                }
                imageViews_.clear();
                images_.clear();
                if (swapchain_ != VK_NULL_HANDLE)
                {
                    vkDestroySwapchainKHR(device_.handle(), swapchain_, nullptr);
                    swapchain_ = VK_NULL_HANDLE;
                }
                format_ = VK_FORMAT_UNDEFINED;
                extent_ = {};
            }

            VkSwapchainKHR VulkanSwapchain::handle() const noexcept
            {
                return swapchain_;
            }

            VkImage VulkanSwapchain::image(uint32_t index) const noexcept
            {
                return images_[index];
            }

            VkImageView VulkanSwapchain::imageView(uint32_t index) const noexcept
            {
                return imageViews_[index];
            }

            uint32_t VulkanSwapchain::imageCount() const noexcept
            {
                return static_cast<uint32_t>(images_.size());
            }

            VkFormat VulkanSwapchain::format() const noexcept
            {
                return format_;
            }

            VkExtent2D VulkanSwapchain::extent() const noexcept
            {
                return extent_;
            }

            VkColorSpaceKHR VulkanSwapchain::colorSpace() const noexcept
            {
                return colorSpace_;
            }
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
