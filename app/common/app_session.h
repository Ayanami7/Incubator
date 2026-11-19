#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "runtime/core/clock/clock.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/platform/window/window.h"
#include "runtime/render/renderer.h"
#include "runtime/resource/asset_source.h"
#include "runtime/scene/scene.h"
#include "session_options.h"
#include "ui_input.h"

struct ImDrawList;

namespace Incubator::App
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

    template <class Application>
    struct SessionCallbacks
    {
        std::function<void(Application&)> initialize;
        std::function<void(Application&, const FrameTime&)> update;
        std::function<void(Application&)> ui;
        std::function<void(Application&)> cancelInteraction;
        std::function<void(Application&, Render::FrameExtent, const App::UiInput&)> input;
        /// @brief 在输入和编辑之前建立当前帧视口布局。
        std::function<void(Application&)> layout;
        /// @brief 窗口收到关闭请求时触发；回调可调用 window().setShouldClose(false) 拦截退出。
        std::function<void(Application&)> closeRequested;
    };

    /// @brief 管理单窗口、主线程的应用会话，并独占所有 Loader 路由。
    class AppSession
    {
    public:
        AppSession(std::string name, const SessionOptions& options, std::unique_ptr<FileIO::FileIO> fileIO);
        ~AppSession();
        AppSession(const AppSession&) = delete;
        AppSession& operator=(const AppSession&) = delete;

        /// @brief 运行单次会话，先释放回调，再关闭会话资源。
        template <class Application>
        void run(Application& app, SessionCallbacks<Application>& callbacks)
        {
            FrameCallbacks frame{
                bind(app, std::move(callbacks.initialize)),     bind(app, std::move(callbacks.update)),
                bind(app, std::move(callbacks.ui)),             bind(app, std::move(callbacks.cancelInteraction)),
                bind(app, std::move(callbacks.input)),          bind(app, std::move(callbacks.layout)),
                bind(app, std::move(callbacks.closeRequested)),
            };
            callbacks = {};
            run(frame);
        }

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
        /// @note 首次在独立视口启用时按需准备深度 Shader。
        Render::ShadowSettings& shadowSettings();
        /// @brief 在调用方打开的 ImGui 窗口中提交场景图像。
        void drawSceneImage();
        void requireOpen() const;
        /// @brief 返回本轮布局确定的场景图像逻辑区域。
        const App::ViewportRegion& viewportRegion() const
        {
            return viewportRegion_;
        }
        Render::FrameExtent sceneExtent() const
        {
            return sceneExtent_;
        }
        /// @brief 返回本帧场景视口窗口的绘制列表；视口未显示或尺寸无效时为空。
        ImDrawList* sceneDrawList() const
        {
            return sceneDrawList_;
        }

    private:
        struct FrameCallbacks
        {
            std::function<void()> initialize;
            std::function<void(const FrameTime&)> update;
            std::function<void()> ui;
            std::function<void()> cancelInteraction;
            std::function<void(Render::FrameExtent, const UiInput&)> input;
            std::function<void()> layout;
            std::function<void()> closeRequested;
        };

        template <class Application, class... Args>
        static std::function<void(Args...)> bind(Application& app, std::function<void(Application&, Args...)> callback)
        {
            if (!callback)
                return {};
            return [&app, callback = std::move(callback)](Args... args) { callback(app, std::forward<Args>(args)...); };
        }

        void run(FrameCallbacks& callbacks);
        void close() noexcept;

        std::string name_;
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
        App::ViewportRegion viewportRegion_;
        ImDrawList* sceneDrawList_ = nullptr;
        bool sceneChanged_ = false;
        bool ownsRoutes_ = false;
        bool exitRequested_ = false;
    };
}  // namespace Incubator::App
