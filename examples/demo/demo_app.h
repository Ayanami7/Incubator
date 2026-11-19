#pragma once

#include "app/common/app_session.h"

namespace Incubator::Demo
{
    class DemoApp;
    using FrameTime = App::FrameTime;
    using Callbacks = App::SessionCallbacks<DemoApp>;

    /// @brief 管理单窗口、主线程的应用会话，并独占所有 Loader 路由。
    class DemoApp
    {
    public:
        explicit DemoApp(App::SessionOptions options = {},
                         std::unique_ptr<FileIO::FileIO> fileIO = FileIO::createPlatformFileIO());
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
        void requestExit()
        {
            session_.requestExit();
        }
        bool isOpen() const
        {
            return session_.isOpen();
        }

        /// @brief 获取当前窗口的借用引用。
        /// @note 仅在会话打开时有效。
        Window::Window& window()
        {
            return session_.window();
        }
        /// @brief 获取当前 Renderer 的借用引用。
        /// @note 仅在会话打开时有效。
        Render::Renderer& renderer()
        {
            return session_.renderer();
        }
        /// @brief 获取当前会话文件服务的借用引用。
        FileIO::FileIO& fileIO()
        {
            return session_.fileIO();
        }
        /// @brief 获取当前 Scene 的借用引用。
        /// @note 仅在会话打开时有效；调用 setScene 后必须重新获取。
        Scene::Scene& scene()
        {
            return session_.scene();
        }
        /// @brief 使用指定场景替换当前场景。
        /// @param scene 新场景。
        void setScene(std::unique_ptr<Scene::Scene> scene)
        {
            session_.setScene(std::move(scene));
        }
        std::array<float, 4>& clearColor()
        {
            return session_.clearColor();
        }
        /// @brief 编辑本会话的阴影预览设置；不参与场景或启动配置的保存加载。
        /// @note 默认关闭，Demo 接收偏移预设为 0.003；启用时仅支持独立视口，首次有效帧按需准备深度 Shader。
        Render::ShadowSettings& shadowSettings()
        {
            return session_.shadowSettings();
        }
        /// @brief 在当前 UI 帧中显示独立场景视口；关闭或隐藏时只提交其他 UI。
        void sceneViewport(const char* title, bool* open = nullptr);
        /// @brief 返回本轮布局确定的场景图像逻辑区域。
        const App::ViewportRegion& viewportRegion() const
        {
            return session_.viewportRegion();
        }
        Render::FrameExtent sceneExtent() const
        {
            return session_.sceneExtent();
        }

    private:
        App::AppSession session_;
    };
}  // namespace Incubator::Demo
