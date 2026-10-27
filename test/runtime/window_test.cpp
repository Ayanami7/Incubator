#define TESTING_ENABLED

#include "runtime/platform/window/window.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

using Incubator::Window::WindowDesc;
using Incubator::Window::Window;
using Incubator::Window::createPlatformWindow;

// ============================================================
//  WindowDesc 测试
// ============================================================

TEST(WindowDescTest, DefaultValues)
{
    WindowDesc desc;

    EXPECT_EQ(desc.title, "Incubator");
    EXPECT_EQ(desc.width, 1280);
    EXPECT_EQ(desc.height, 720);
    EXPECT_TRUE(desc.resizable);
    EXPECT_TRUE(desc.visible);
}

// ============================================================
//  WindowTest 测试夹具
// ============================================================

class WindowTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        WindowDesc desc;
        desc.title  = "WindowTest";
        desc.width  = 640;
        desc.height = 480;

        try
        {
            window_ = createPlatformWindow(desc);
        }
        catch (...)
        {
            GTEST_SKIP() << "Cannot create GLFW window (no display?)";
        }
    }

    void TearDown() override
    {
        window_.reset();
    }

#ifdef _WIN32
    std::wstring getWindowTitle() const
    {
        auto* native = static_cast<GLFWwindow*>(window_->getNativeHandle());
        const HWND handle = glfwGetWin32Window(native);
        const int length = GetWindowTextLengthW(handle);
        std::wstring title(static_cast<size_t>(length) + 1, L'\0');
        const int written = GetWindowTextW(handle, title.data(), static_cast<int>(title.size()));
        title.resize(static_cast<size_t>(written));
        return title;
    }
#endif

    std::unique_ptr<Window> window_;
};

// ============================================================
//  工厂与生命周期测试（通过测试夹具）
// ============================================================

TEST_F(WindowTest, ShouldCloseInitiallyFalse)
{
    ASSERT_NE(window_, nullptr);
    EXPECT_FALSE(window_->shouldClose());
}

TEST_F(WindowTest, SetShouldCloseTrue)
{
    ASSERT_NE(window_, nullptr);

    window_->setShouldClose(true);
    EXPECT_TRUE(window_->shouldClose());
}

TEST_F(WindowTest, SetShouldCloseFalse)
{
    ASSERT_NE(window_, nullptr);

    window_->setShouldClose(true);
    window_->setShouldClose(false);
    EXPECT_FALSE(window_->shouldClose());
}

TEST_F(WindowTest, PollEventsDoesNotCrash)
{
    ASSERT_NE(window_, nullptr);

    for (int i = 0; i < 10; ++i)
    {
        window_->pollEvents();
    }
}

TEST_F(WindowTest, GetFramebufferSizeReturnsPositiveValues)
{
    ASSERT_NE(window_, nullptr);

    int width  = 0;
    int height = 0;
    window_->getFramebufferSize(width, height);

    EXPECT_GT(width, 0);
    EXPECT_GT(height, 0);
}

TEST_F(WindowTest, GetWindowSizeMatchesRequested)
{
    ASSERT_NE(window_, nullptr);

    int width  = 0;
    int height = 0;
    window_->getWindowSize(width, height);

    EXPECT_EQ(width, 640);
    EXPECT_EQ(height, 480);
}

TEST_F(WindowTest, GetNativeHandleReturnsNonNull)
{
    ASSERT_NE(window_, nullptr);

    void* handle = window_->getNativeHandle();
    EXPECT_NE(handle, nullptr);
}

#ifdef _WIN32
TEST_F(WindowTest, SetTitleUsesSubstringLength)
{
    const std::string backing = "Title trailing text";

    window_->setTitle(std::string_view(backing).substr(0, 5));

    EXPECT_EQ(getWindowTitle(), L"Title");
}

TEST_F(WindowTest, SetTitleAcceptsNonNullTerminatedBuffer)
{
    const char title[] = {'T', 'i', 't', 'l', 'e'};

    window_->setTitle(std::string_view(title, sizeof(title)));

    EXPECT_EQ(getWindowTitle(), L"Title");
}

TEST_F(WindowTest, SetTitleAcceptsEmptyView)
{
    window_->setTitle(std::string_view{});

    EXPECT_EQ(getWindowTitle(), L"");
}

TEST_F(WindowTest, SetTitlePreservesUtf8Chinese)
{
    window_->setTitle("纹理加载窗口");

    EXPECT_EQ(getWindowTitle(), L"纹理加载窗口");
}
#endif

// ============================================================
//  工厂函数独立测试
// ============================================================

TEST(WindowFactoryTest, CreateWithDefaultDesc)
{
    WindowDesc desc;
    std::unique_ptr<Window> window;

    try
    {
        window = createPlatformWindow(desc);
    }
    catch (...)
    {
        GTEST_SKIP() << "Cannot create GLFW window (no display?)";
    }

    ASSERT_NE(window, nullptr);
}
