#pragma once

#include "app/common/app_session.h"

namespace Incubator::Editor
{
    class EditorApp;
    using FrameTime = App::FrameTime;
    using Callbacks = App::SessionCallbacks<EditorApp>;

    /// @brief 管理单窗口、主线程的应用会话，并独占所有 Loader 路由。
    class EditorApp
    {
    public:
        explicit EditorApp(App::SessionOptions options = {},
                           std::unique_ptr<FileIO::FileIO> fileIO = FileIO::createPlatformFileIO());
        EditorApp(const EditorApp&) = delete;
        EditorApp& operator=(const EditorApp&) = delete;
        EditorApp(EditorApp&&) = delete;
        EditorApp& operator=(EditorApp&&) = delete;

        /// @brief 运行编辑主循环。
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
        /// @note 默认关闭，Editor 接收偏移预设为 0.003；启用时仅支持独立视口，首次有效帧按需准备深度 Shader。
        Render::ShadowSettings& shadowSettings()
        {
            return session_.shadowSettings();
        }
        /// @brief 编辑本会话的 FXAA 预览开关；默认关闭，不参与场景或启动配置的保存加载。
        /// @note 首次在独立视口启用时按需准备片元 Shader；UI 阶段改动从下一有效视口帧生效。
        bool& fxaaEnabled()
        {
            return session_.fxaaEnabled();
        }
        bool fxaaEnabled() const
        {
            return session_.fxaaEnabled();
        }
        /// @brief 返回本轮布局准备的场景输出引用，供同一帧采样与提交共用。
        const Render::SceneOutput& sceneOutput() const
        {
            return session_.sceneOutput();
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
        /// @brief 返回本帧场景视口窗口的绘制列表；视口未显示或尺寸无效时为空。
        ImDrawList* sceneDrawList() const
        {
            return session_.sceneDrawList();
        }
        /// @brief 返回当前主循环最近一帧的 CPU 帧时间。
        const FrameTime& lastFrameTime() const
        {
            return session_.lastFrameTime();
        }
        /// @brief 返回最近一次已完成的离屏视口帧统计样本。
        Render::ViewportStats viewportStats() const
        {
            return session_.viewportStats();
        }

    private:
        App::AppSession session_;
    };
}  // namespace Incubator::Editor
