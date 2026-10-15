#include "runtime/platform/window/window.h"

#include <GLFW/glfw3.h>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"

namespace Incubator
{
    namespace Window
    {
        namespace
        {

            // ============================================================
            //  GLFW生命周期引用计数
            // ============================================================

            static int s_glfwRefCount = 0;

            void glfwErrorCallback(int error, const char* description)
            {
                Log::Registry::get("platform.window").error("GLFW error {}: {}", error, description);
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

                    if (s_glfwRefCount++ == 0)
                    {
                        if (!glfwInit())
                        {
                            s_glfwRefCount = 0;
                            throw Incubator::Error::Exception(
                                Incubator::Error::Code::IO,
                                "GLFW initialization failed"
                            );
                        }
                    }

                    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
                    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);
                    glfwWindowHint(GLFW_VISIBLE, desc.visible ? GLFW_TRUE : GLFW_FALSE);

                    window_ = glfwCreateWindow(
                        desc.width, desc.height, desc.title.c_str(), nullptr, nullptr
                    );

                    if (!window_)
                    {
                        if (--s_glfwRefCount == 0)
                        {
                            glfwTerminate();
                        }
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
                    if (--s_glfwRefCount == 0)
                    {
                        glfwTerminate();
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
                    const std::string terminatedTitle(title);
                    glfwSetWindowTitle(window_, terminatedTitle.c_str());
                }

                // ---- 窗口状态查询 ----

                bool isIconified() const override
                {
                    return glfwGetWindowAttrib(window_, GLFW_ICONIFIED) != 0;
                }

                float getContentScale() const override
                {
                    float x, y;
                    glfwGetWindowContentScale(window_, &x, &y);
                    return x;
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
            auto window = std::make_unique<WindowGlfw>(desc);
            int width = 0;
            int height = 0;
            int framebufferWidth = 0;
            int framebufferHeight = 0;
            window->getWindowSize(width, height);
            window->getFramebufferSize(framebufferWidth, framebufferHeight);
            Log::Registry::get("platform.window").debug(
                "Created window '{}': {}x{}, framebuffer {}x{}, content scale {}, resizable={}, visible={}",
                desc.title, width, height, framebufferWidth, framebufferHeight, window->getContentScale(),
                desc.resizable, desc.visible);
            return window;
        }

    }  // namespace Window
}  // namespace Incubator
