#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace Incubator
{
    namespace Window
    {
        class Window;
    }

    namespace Render
    {
        namespace Integration
        {
            class ImGuiRenderer
            {
            public:
                explicit ImGuiRenderer(Window::Window& window);
                ~ImGuiRenderer();

                ImGuiRenderer(const ImGuiRenderer&) = delete;
                ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

                void initializeVulkan(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
                                      uint32_t queueFamily, VkQueue queue, VkFormat colorFormat,
                                      VkFormat depthFormat, uint32_t imageCount);
                void beginFrame();
                void endFrame();
                void* drawData() const noexcept;
                void recordDrawData(void* drawData, void* nativeCommandBuffer) const;

            private:
                void shutdownVulkan() noexcept;
                Window::Window& window_;
                VkInstance instance_ = VK_NULL_HANDLE;
                VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
                VkDevice device_ = VK_NULL_HANDLE;
                uint32_t queueFamily_ = VK_QUEUE_FAMILY_IGNORED;
                VkQueue queue_ = VK_NULL_HANDLE;
                VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
                VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
                uint32_t imageCount_ = 0;
                bool contextCreated_ = false;
                bool glfwInitialized_ = false;
                bool vulkanInitialized_ = false;
            };
        }  // namespace Integration
    }  // namespace Render
}  // namespace Incubator
