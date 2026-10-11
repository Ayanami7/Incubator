#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            class GlfwSurface;
            class VulkanDevice;

            class VulkanSwapchain
            {
            public:
                VulkanSwapchain(const VulkanDevice& device, const GlfwSurface& surface, int width, int height);
                ~VulkanSwapchain();

                VulkanSwapchain(const VulkanSwapchain&) = delete;
                VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

                void recreate(int width, int height);
                VkSwapchainKHR handle() const noexcept;
                VkImage image(uint32_t index) const noexcept;
                VkImageView imageView(uint32_t index) const noexcept;
                uint32_t imageCount() const noexcept;
                VkFormat format() const noexcept;
                VkExtent2D extent() const noexcept;

            private:
                void destroy() noexcept;

                const VulkanDevice& device_;
                const GlfwSurface& surface_;
                VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
                VkFormat format_ = VK_FORMAT_UNDEFINED;
                VkExtent2D extent_{};
                std::vector<VkImage> images_;
                std::vector<VkImageView> imageViews_;
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
