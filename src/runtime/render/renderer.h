#pragma once

#include <array>
#include <functional>
#include <memory>

namespace Incubator
{
    namespace Window
    {
        class Window;
    }

    namespace Resource
    {
        class ShaderResource;
        class TextureResource;
    }  // namespace Resource

    namespace Render
    {
        namespace Integration
        {
            class ImGuiRenderer;
        }

        enum class RendererValidationMode
        {
            Disabled,
            Optional,
            Required
        };

        class Renderer
        {
        public:
            Renderer(Window::Window& window, const Resource::ShaderResource& vertexShader,
                     const Resource::ShaderResource& fragmentShader, const Resource::TextureResource& texture,
                     RendererValidationMode validationMode = RendererValidationMode::Disabled);
            ~Renderer();

            Renderer(const Renderer&) = delete;
            Renderer& operator=(const Renderer&) = delete;

            Renderer(Renderer&&) noexcept;
            Renderer& operator=(Renderer&&) noexcept;

            // Prepares swapchain/backend before generating UI data; false skips this UI frame.
            bool beginUiFrame();

            // The callback runs inside the active color/depth rendering scope. Its opaque
            // argument is the native command buffer, valid only for this invocation.
            // clearColor RGB is sRGB encoded; alpha is linear, as with the ImGui backend.
            void drawFrame(const std::array<float, 4>& clearColor, const std::function<void(void*)>& recordUi = {});
            Integration::ImGuiRenderer& imgui();

        private:
            class Impl;
            std::unique_ptr<Impl> impl_;
        };
    }  // namespace Render
}  // namespace Incubator
