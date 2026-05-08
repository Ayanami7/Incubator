#include "runtime/platform/window/window.h"

#include <GLFW/glfw3.h>

#include <spdlog/spdlog.h>

#include "runtime/core/error/error.h"

namespace Incubator
{
    namespace Window
    {
        namespace
        {

            // ============================================================
            //  GLFW Error Callback
            // ============================================================

            void glfwErrorCallback(int error, const char* description)
            {
                spdlog::error("[GLFW] Error {}: {}", error, description);
            }

            // ============================================================
            //  WindowGlfw — concrete GLFW implementation of Window
            // ============================================================

            class WindowGlfw : public Window
            {
            public:
                explicit WindowGlfw(const WindowDesc& desc)
                {
                    glfwSetErrorCallback(glfwErrorCallback);

                    if (!glfwInit())
                    {
                        throw Incubator::Error::Exception(
                            Incubator::Error::Code::IO,
                            "GLFW initialization failed"
                        );
                    }

                    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
                    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);
                    glfwWindowHint(GLFW_VISIBLE, desc.visible ? GLFW_TRUE : GLFW_FALSE);

                    window_ = glfwCreateWindow(
                        desc.width, desc.height, desc.title.c_str(), nullptr, nullptr
                    );

                    if (!window_)
                    {
                        glfwTerminate();
                        throw Incubator::Error::Exception(
                            Incubator::Error::Code::IO,
                            "GLFW window creation failed"
                        );
                    }
                }

                ~WindowGlfw() override
                {
                    if (window_)
                    {
                        glfwDestroyWindow(window_);
                        window_ = nullptr;
                    }
                }

                // ---- 窗口生命周期 ----

                bool shouldClose() const override
                {
                    return glfwWindowShouldClose(window_) != 0;
                }

                void setShouldClose(bool value) override
                {
                    glfwSetWindowShouldClose(window_, value ? GLFW_TRUE : GLFW_FALSE);
                }

                // ---- 事件处理 ----

                void pollEvents() override
                {
                    glfwPollEvents();
                }

                // ---- 尺寸查询 ----

                void getFramebufferSize(int& width, int& height) const override
                {
                    glfwGetFramebufferSize(window_, &width, &height);
                }

                void getWindowSize(int& width, int& height) const override
                {
                    glfwGetWindowSize(window_, &width, &height);
                }

                // ---- 窗口控制 ----

                void setTitle(std::string_view title) override
                {
                    glfwSetWindowTitle(window_, title.data());
                }

                void* getNativeHandle() const override
                {
                    return static_cast<void*>(window_);
                }

            private:
                GLFWwindow* window_ = nullptr;
            };

        }  // anonymous namespace

        // ============================================================
        //  Factory Function
        // ============================================================

        std::unique_ptr<Window> createPlatformWindow(const WindowDesc& desc)
        {
            return std::make_unique<WindowGlfw>(desc);
        }

    }  // namespace Window
}  // namespace Incubator
