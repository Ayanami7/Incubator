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

            /// @brief 同步复制非空的三角形列表数据。
            /// @param vertices 顶点数据，仅在本次调用期间借用。
            /// @param indices 索引数据，仅在本次调用期间借用。
            /// @return 由此 Renderer 持有的网格引用。
            /// @note 返回的 Mesh 仅可与此 Renderer 配合使用。
            /// @note normal 为有限的几何局部法线，可为零或非单位向量；非零法线上传前归一化，顶点阶段变换后再次归一化。
            /// @throws Error::Exception 法线非有限或顶点/索引不满足三角形列表约定。
            Mesh createMesh(std::span<const MeshVertex> vertices, std::span<const uint32_t> indices);

            /// @brief 同步上传 RGBA8 sRGB 像素。
            /// @param texture CPU 纹理，仅在本次调用期间借用。
            /// @param mipMode mip 链生成方式。
            /// @return 由此 Renderer 持有的纹理引用。
            /// @note FullChain 会在返回前生成完整 GPU mip 链；默认只上传第 0 级。
            /// 返回的 Texture 仅可用于此 Renderer。
            Texture createTexture(const Resource::TextureResource& texture,
                                  TextureMipMode mipMode = TextureMipMode::BaseLevel);

            /// @brief 准备共享场景着色器以及帧和材质布局。
            /// @param vertexShader 顶点着色器资源。
            /// @param fragmentShader 片段着色器资源。
            /// @note 复制着色器数据以支持兼容的 Pipeline 重建；不保留 CPU 资源。空帧或仅绘制 UI 时无需调用。
            void prepareSceneResources(const Resource::ShaderResource& vertexShader,
                                       const Resource::ShaderResource& fragmentShader);

            /// @brief 根据描述创建不可变材质绑定。
            /// @param desc 材质描述，仅在本次调用期间借用。
            /// @return 由此 Renderer 持有的材质引用。
            /// @note 需要先准备场景着色器，并传入由此 Renderer 创建的有效 Texture；参数会被复制。
            Material createMaterial(const MaterialDesc& desc);

            /// @brief 准备交换链和后端，并开始 UI 帧。
            /// @return 实际渲染尺寸；无可用尺寸时返回空值并跳过本帧。
            std::optional<FrameExtent> beginUiFrame();

            /// @brief 提交场景帧并在当前渲染通道中记录 UI。
            /// @param input 场景帧输入；尺寸变化后过期的输入会被跳过，应在下一帧重新生成。
            /// @param clearColor 清屏颜色；RGB 使用 sRGB 编码，alpha 为线性值，与 ImGui 后端一致。
            /// @param recordUi 在活动颜色/深度渲染作用域内调用的 UI 记录回调。
            /// @note 回调参数是原生命令缓冲区，仅在本次回调期间有效。
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
