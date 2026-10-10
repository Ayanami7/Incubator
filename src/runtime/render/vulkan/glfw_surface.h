#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace Incubator
{
    namespace Window
    {
        class Window;
    }

    namespace Render
    {
        namespace Vulkan
        {
            class VulkanInstance;

            class GlfwSurface
            {
            public:
                static std::vector<const char*> requiredInstanceExtensions();

                GlfwSurface(const VulkanInstance& instance, Window::Window& window);
                ~GlfwSurface();

                GlfwSurface(const GlfwSurface&) = delete;
                GlfwSurface& operator=(const GlfwSurface&) = delete;

                VkSurfaceKHR handle() const noexcept;

            private:
                const VulkanInstance& instance_;
                VkSurfaceKHR surface_ = VK_NULL_HANDLE;
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
