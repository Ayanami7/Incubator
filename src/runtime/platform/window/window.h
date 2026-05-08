#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace Incubator
{
    namespace Window
    {
        /// @brief 窗口创建描述符
        ///
        /// 用于向 createPlatformWindow() 工厂函数传递窗口初始化参数。
        struct WindowDesc
        {
            /// @brief 窗口标题
            std::string title = "Incubator";

            /// @brief 窗口宽度（像素）
            int32_t width = 1280;

            /// @brief 窗口高度（像素）
            int32_t height = 720;

            /// @brief 是否允许用户调整窗口大小
            bool resizable = true;

            /// @brief 创建后是否立即可见
            bool visible = true;
        };

        /// @brief 窗口抽象基类
        ///
        /// 封装平台相关的窗口创建与事件轮询接口，提供可 Mock 的抽象。
        /// 使用 createPlatformWindow() 工厂函数获取平台实例。
        /// 平台细节（GLFW 等）对用户完全隐藏，通过 getNativeHandle() 按需暴露。
        class Window
        {
        public:
            virtual ~Window() = default;

            // ---- 窗口生命周期 ----

            /// @brief 窗口是否请求关闭
            /// @return 用户点击关闭按钮或调用 setShouldClose(true) 后返回 true
            virtual bool shouldClose() const = 0;

            /// @brief 设置窗口关闭请求标志
            /// @param value true 标记窗口待关闭，false 清除关闭标记
            virtual void setShouldClose(bool value) = 0;

            // ---- 事件处理 ----

            /// @brief 轮询并分发待处理的窗口事件
            ///
            /// 内部调用 glfwPollEvents()，触发已注册的回调。
            /// 应在每帧主循环中调用一次。
            virtual void pollEvents() = 0;

            // ---- 尺寸查询 ----

            /// @brief 获取帧缓冲区尺寸（像素）
            /// @param[out] width  帧缓冲宽度
            /// @param[out] height 帧缓冲高度
            ///
            /// 帧缓冲区尺寸可能与窗口尺寸不同（例如在 HiDPI 显示器上）。
            /// 通常用于设置视口和交换链尺寸。
            virtual void getFramebufferSize(int& width, int& height) const = 0;

            /// @brief 获取窗口尺寸（屏幕坐标）
            /// @param[out] width  窗口宽度
            /// @param[out] height 窗口高度
            virtual void getWindowSize(int& width, int& height) const = 0;

            // ---- 窗口控制 ----

            /// @brief 设置窗口标题
            /// @param title 新标题
            virtual void setTitle(std::string_view title) = 0;

            /// @brief 获取原生窗口句柄
            /// @return GLFWwindow*（以 void* 形式返回）
            ///
            /// 用于需要直接调用 GLFW API 的底层操作（如输入查询、上下文管理）。
            virtual void* getNativeHandle() const = 0;

            /// @brief 禁止拷贝
            Window(const Window&) = delete;
            Window& operator=(const Window&) = delete;

            /// @brief 允许移动
            Window(Window&&) noexcept = default;
            Window& operator=(Window&&) noexcept = default;

        protected:
            Window() = default;
        };

        /// @brief 创建平台相关的窗口实例
        /// @param desc 窗口创建参数
        /// @return 窗口对象指针；创建失败时抛出异常
        std::unique_ptr<Window> createPlatformWindow(const WindowDesc& desc);

    }  // namespace Window
}  // namespace Incubator
