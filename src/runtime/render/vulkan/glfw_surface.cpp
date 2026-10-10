#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "runtime/render/vulkan/glfw_surface.h"

#include "runtime/core/error/error.h"
#include "runtime/render/vulkan/vulkan_instance.h"
#include "runtime/platform/window/window.h"

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

            std::vector<const char*> GlfwSurface::requiredInstanceExtensions()
            {
                if (glfwVulkanSupported() != GLFW_TRUE)
                {
                    throw Error::Exception(Error::Code::RENDER, "GLFW reports that Vulkan is unavailable");
                }

                uint32_t extensionCount = 0;
                const char** extensions = glfwGetRequiredInstanceExtensions(&extensionCount);
                if (extensions == nullptr || extensionCount == 0)
                {
                    throw Error::Exception(Error::Code::RENDER,
                                           "GLFW did not provide Vulkan instance extensions");
                }
                return {extensions, extensions + extensionCount};
            }

            GlfwSurface::GlfwSurface(const VulkanInstance& instance, Window::Window& window) : instance_(instance)
            {
                const auto nativeWindow = static_cast<GLFWwindow*>(window.getNativeHandle());
                checkVk(glfwCreateWindowSurface(instance_.handle(), nativeWindow, nullptr, &surface_),
                        "glfwCreateWindowSurface");
            }

            GlfwSurface::~GlfwSurface()
            {
                if (surface_ != VK_NULL_HANDLE)
                {
                    vkDestroySurfaceKHR(instance_.handle(), surface_, nullptr);
                }
            }

            VkSurfaceKHR GlfwSurface::handle() const noexcept
            {
                return surface_;
            }
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
