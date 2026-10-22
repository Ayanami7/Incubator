#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>

#include "runtime/render/frame_input.h"
#include "runtime/render/material.h"
#include "runtime/render/mesh.h"
#include "runtime/render/texture.h"

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
        namespace Detail
        {
            struct RendererReviewAccess;
        }
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

            // Synchronously copies nonempty triangle-list data. CPU spans are borrowed only here.
            // Mesh references share this upload and are usable only with this Renderer.
            Mesh createMesh(std::span<const MeshVertex> vertices, std::span<const uint32_t> indices);

            // Synchronously uploads RGBA8 sRGB pixels, borrowed only for this call.
            // Texture references share this upload and are usable only with this Renderer.
            Texture createTexture(const Resource::TextureResource& texture);

            // Prepares the shared scene shaders and frame/material layouts once.
            // Copies shader words for compatible Pipeline rebuilds; no CPU resource is retained.
            // Empty frames/UI do not require this call.
            void prepareSceneResources(const Resource::ShaderResource& vertexShader,
                                       const Resource::ShaderResource& fragmentShader);

            // Requires prepared scene shaders and a valid Texture from this Renderer.
            // Copies parameters and creates immutable bindings; borrows desc only for this call.
            Material createMaterial(const MaterialDesc& desc);

            // Prepares the swapchain/backend and starts UI. No extent means skip the frame.
            // The returned nonzero extent is the actual rendering extent for application projection.
            std::optional<FrameExtent> beginUiFrame();

            // The callback runs inside the active color/depth rendering scope. Its opaque
            // argument is the native command buffer, valid only for this invocation.
            // clearColor RGB is sRGB encoded; alpha is linear, as with the ImGui backend.
            // An extent changed since beginUiFrame skips stale input; regenerate it next frame.
            void drawFrame(const FrameInput& input, const std::array<float, 4>& clearColor,
                           const std::function<void(void*)>& recordUi = {});
            Integration::ImGuiRenderer& imgui();

        private:
            friend struct Detail::RendererReviewAccess;
            class Impl;
            std::unique_ptr<Impl> impl_;
        };
    }  // namespace Render
}  // namespace Incubator
