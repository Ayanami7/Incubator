#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <vector>

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
                /// @brief 开始 ImGui 帧。
                /// @note 需要先调用 initializeVulkan；应用代码应使用 Renderer::beginUiFrame。
                /// 在新帧开始前检测 DPI 变化，必要时同步重建字体与样式。
                void beginFrame();
                /// @brief 为已完成等待的帧槽注册场景图像，返回 ImGui 纹理标识。
                uint64_t sceneImage(uint32_t slot, VkImageView image);
                /// @brief 释放已完成等待的帧槽场景绑定。
                void releaseSceneImage(uint32_t slot);
                void endFrame();
                void* drawData() const noexcept;
                void recordDrawData(void* drawData, void* nativeCommandBuffer) const;

            private:
                struct SceneBinding
                {
                    VkImageView image = VK_NULL_HANDLE;
                    VkDescriptorSet set = VK_NULL_HANDLE;
                };
                std::array<SceneBinding, 2> sceneBindings_{};
                VkSampler sceneSampler_ = VK_NULL_HANDLE;
                void updateDpi();
                void shutdownVulkan() noexcept;
                Window::Window& window_;
                std::vector<uint8_t> fontData_;
                float contentScale_ = 0;
                float framebufferScale_ = 0;
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
