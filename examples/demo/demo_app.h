#pragma once

#include <array>
#include <functional>
#include <memory>

#include "demo_options.h"
#include "runtime/core/clock/clock.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/platform/window/window.h"
#include "runtime/render/renderer.h"
#include "runtime/resource/asset_source.h"
#include "runtime/scene/scene.h"
#include "ui_input.h"

namespace Incubator::Demo
{
    struct FrameTime
    {
        Clock::TimePoint now;
        Clock::Duration delta;

        double deltaSeconds() const
        {
            return std::chrono::duration<double>(delta).count();
        }
    };

    class DemoApp;

    struct Callbacks
    {
        std::function<void(DemoApp&)> initialize;
        std::function<void(DemoApp&, const FrameTime&)> update;
        std::function<void(DemoApp&)> ui;
        std::function<void(DemoApp&)> cancelInteraction;
        std::function<void(DemoApp&, Render::FrameExtent, const UiInput&)> input;
        /// @brief 在输入和编辑之前建立当前帧视口布局。
        std::function<void(DemoApp&)> layout;
    };

    /// @brief 管理单窗口、主线程的应用会话，并独占所有 Loader 路由。
    class DemoApp
    {
    public:
        explicit DemoApp(DemoOptions options = {},
                         std::unique_ptr<FileIO::FileIO> fileIO = FileIO::createPlatformFileIO());
        ~DemoApp();
        DemoApp(const DemoApp&) = delete;
        DemoApp& operator=(const DemoApp&) = delete;
        DemoApp(DemoApp&&) = delete;
        DemoApp& operator=(DemoApp&&) = delete;

        /// @brief 运行模型展示主循环。
        void run();
        /// @brief 使用指定回调运行应用循环。
        /// @param callbacks 应用初始化、更新、输入和 UI 回调。
        /// @note run
        /// 仅允许执行一次且不可重入。正常返回或回调失败都会结束会话；错误交由调用方记录日志，且关闭前会先释放回调对象。
        /// 外部 CPU 资源持有须在会话结束前释放，随后才能解除资产源绑定并销毁源及文件服务。
        void run(Callbacks callbacks);
        void requestExit();
        bool isOpen() const;

        /// @brief 获取当前窗口的借用引用。
        /// @note 仅在会话打开时有效。
        Window::Window& window();
        /// @brief 获取当前 Renderer 的借用引用。
        /// @note 仅在会话打开时有效。
        Render::Renderer& renderer();
        /// @brief 获取当前会话文件服务的借用引用。
        FileIO::FileIO& fileIO();
        /// @brief 获取当前 Scene 的借用引用。
        /// @note 仅在会话打开时有效；调用 setScene 后必须重新获取。
        Scene::Scene& scene();
        /// @brief 使用指定场景替换当前场景。
        /// @param scene 新场景。
        void setScene(std::unique_ptr<Scene::Scene> scene);
        std::array<float, 4>& clearColor();
        /// @brief 编辑本会话的阴影预览设置；不参与场景或启动配置的保存加载。
        /// @note 默认关闭，Demo 接收偏移预设为 0.003；启用时仅支持独立视口，首次有效帧按需准备深度 Shader。
        Render::ShadowSettings& shadowSettings();
        /// @brief 在当前 UI 帧中显示独立场景视口；关闭或隐藏时只提交其他 UI。
        void sceneViewport(const char* title, bool* open = nullptr);
        /// @brief 返回本轮布局确定的场景图像逻辑区域。
        const ViewportRegion& viewportRegion() const
        {
            return viewportRegion_;
        }
        Render::FrameExtent sceneExtent() const
        {
            return sceneExtent_;
        }

    private:
        void requireOpen() const;
        void close() noexcept;

        std::unique_ptr<FileIO::FileIO> fileIO_;
        std::unique_ptr<Resource::AssetSource> assetSource_;
        std::unique_ptr<Window::Window> window_;
        std::unique_ptr<Render::Renderer> renderer_;
        std::unique_ptr<Scene::Scene> scene_;
        std::array<float, 4> clearColor_;
        Render::ShadowSettings shadowSettings_{};
        bool shadowResourcesPrepared_ = false;
        Render::FrameExtent sceneExtent_{};
        Render::SceneOutput sceneOutput_;
        ViewportRegion viewportRegion_;
        bool sceneChanged_ = false;
        bool ownsRoutes_ = false;
        bool exitRequested_ = false;
    };
}  // namespace Incubator::Demo
