#pragma once

#include <array>
#include <memory>

namespace Incubator
{
    namespace Window
    {
        class Window;
    }

    namespace Render
    {
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

            void drawFrame(const std::array<float, 4>& clearColor);

        private:
            class Impl;
            std::unique_ptr<Impl> impl_;
        };
    }  // namespace Render
}  // namespace Incubator
