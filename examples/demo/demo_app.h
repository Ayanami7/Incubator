#pragma once

#include <array>
#include <functional>
#include <memory>

#include "demo_options.h"
#include "runtime/core/clock/clock.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/platform/window/window.h"
#include "runtime/render/renderer.h"
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
        /// @note run 仅允许执行一次且不可重入。正常返回或回调失败都会结束会话；错误交由调用方记录日志，且关闭前会先释放回调对象。
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

    private:
        void requireOpen() const;
        void close() noexcept;

        std::unique_ptr<FileIO::FileIO> fileIO_;
        std::unique_ptr<Window::Window> window_;
        std::unique_ptr<Render::Renderer> renderer_;
        std::unique_ptr<Scene::Scene> scene_;
        std::array<float, 4> clearColor_;
        bool ownsRoutes_ = false;
        bool exitRequested_ = false;
    };
}  // namespace Incubator::Demo
