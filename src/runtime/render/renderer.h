#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>

#include "runtime/render/frame_input.h"
#include "runtime/render/mesh.h"

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

            // Synchronously copies nonempty triangle-list data. CPU spans are borrowed only here.
            // Mesh references share this upload and are usable only with this Renderer.
            Mesh createMesh(std::span<const MeshVertex> vertices, std::span<const uint16_t> indices);

            // Prepares the swapchain/backend and starts UI. No extent means skip the frame.
            // The returned nonzero extent is the actual rendering extent for application projection.
            std::optional<FrameExtent> beginUiFrame();

            // The callback runs inside the active color/depth rendering scope. Its opaque
            // argument is the native command buffer, valid only for this invocation.
            // clearColor RGB is sRGB encoded; alpha is linear, as with the ImGui backend.
            // An extent changed since beginUiFrame skips stale input; regenerate it next frame.
            void drawFrame(const FrameInput& input, const Mesh& mesh, const std::array<float, 4>& clearColor,
                           const std::function<void(void*)>& recordUi = {});
            Integration::ImGuiRenderer& imgui();

        private:
            class Impl;
            std::unique_ptr<Impl> impl_;
        };
    }  // namespace Render
}  // namespace Incubator
