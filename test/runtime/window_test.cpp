#define TESTING_ENABLED

#include "runtime/platform/window/window.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using Incubator::Window::WindowDesc;
using Incubator::Window::Window;
using Incubator::Window::createPlatformWindow;

// ============================================================
//  WindowDesc Tests
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

TEST(WindowDescTest, CustomValues)
{
    WindowDesc desc;
    desc.title     = "Custom Window";
    desc.width     = 800;
    desc.height    = 600;
    desc.resizable = false;
    desc.visible   = false;

    EXPECT_EQ(desc.title, "Custom Window");
    EXPECT_EQ(desc.width, 800);
    EXPECT_EQ(desc.height, 600);
    EXPECT_FALSE(desc.resizable);
    EXPECT_FALSE(desc.visible);
}

// ============================================================
//  WindowTest Fixture
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

    std::unique_ptr<Window> window_;
};

// ============================================================
//  Factory & Lifecycle Tests (via Fixture)
// ============================================================

TEST_F(WindowTest, FactoryReturnsNonNull)
{
    ASSERT_NE(window_, nullptr);
}

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

TEST_F(WindowTest, SetTitleDoesNotCrash)
{
    ASSERT_NE(window_, nullptr);

    window_->setTitle("New Title");
    window_->setTitle("Another Title");
    window_->setTitle("");
}

// ============================================================
//  Factory Function — Standalone Test
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
