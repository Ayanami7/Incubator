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
            explicit Renderer(Window::Window& window,
                              RendererValidationMode validationMode = RendererValidationMode::Disabled);
            ~Renderer();

            Renderer(const Renderer&) = delete;
            Renderer& operator=(const Renderer&) = delete;

            Renderer(Renderer&&) noexcept;
            Renderer& operator=(Renderer&&) noexcept;

            // The callback runs inside the active color/depth rendering scope. Its opaque
            // argument is the native command buffer, valid only for this invocation.
            void drawFrame(const std::array<float, 4>& clearColor,
                           const std::function<void(void*)>& recordUi = {});
            Integration::ImGuiRenderer& imgui();

        private:
            class Impl;
            std::unique_ptr<Impl> impl_;
        };
    }  // namespace Render
}  // namespace Incubator
