#include <gtest/gtest.h>
#include <imgui.h>
#include <spdlog/sinks/ostream_sink.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/platform/window/window.h"
#include "runtime/render/integration/imgui_renderer.h"
#include "runtime/render/renderer.h"
#include "support/window_capture.h"

namespace
{
    using namespace Incubator;

    /// @brief 复用真实窗口，只覆盖受控测试的 DPI 与像素密度，不修改系统显示设置。
    class DpiWindow final : public Window::Window
    {
    public:
        explicit DpiWindow(Incubator::Window::Window& native) : native_(native) {}
        bool shouldClose() const override { return native_.shouldClose(); }
        void setShouldClose(bool close) override { native_.setShouldClose(close); }
        void pollEvents() override { native_.pollEvents(); }
        void beginInputFrame() override { native_.beginInputFrame(); }
        const Input::InputState& input() const override { return native_.input(); }
        void getFramebufferSize(int& width, int& height) const override
        {
            native_.getFramebufferSize(width, height);
            width *= density;
            height *= density;
        }
        void getWindowSize(int& width, int& height) const override { native_.getWindowSize(width, height); }
        void setTitle(std::string_view title) override { native_.setTitle(title); }
        bool isIconified() const override { return native_.isIconified(); }
        float getContentScale() const override { return scale > 0 ? scale : native_.getContentScale(); }
        void* getNativeHandle() const override { return native_.getNativeHandle(); }
        float scale = 0;
        int density = 1;

    private:
        Incubator::Window::Window& native_;
    };

    class ImGuiIntegrationTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            previousPath_ = std::filesystem::current_path();
            std::filesystem::current_path(Utils::Utf8::toFsPath(RUNTIME_TEST_ASSET_DIR).parent_path());
            Log::Registry::reset();
            auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(messages_);
            sink->set_pattern("%l|%v");
            Log::Registry::get("render").addSink(sink);
            Log::Registry::get("render.vulkan").addSink(sink);
        }
        void TearDown() override
        {
            Log::Registry::reset();
            std::filesystem::current_path(previousPath_);
        }

        std::unique_ptr<Window::Window> window(bool visible = false)
        {
            Window::WindowDesc desc;
            desc.title = "Incubator font and DPI review";
            desc.width = 1000;
            desc.height = 620;
            desc.visible = visible;
            return Window::createPlatformWindow(desc);
        }

        void render(Render::Renderer& renderer)
        {
            const auto extent = renderer.beginUiFrame();
            ASSERT_TRUE(extent);
            ImGui::SetNextWindowPos({12, 12});
            ImGui::SetNextWindowSize({940, 550});
            ImGui::Begin("Font and DPI");
            ImGui::TextUnformatted("Scene / Inspector / Position / 0123456789");
            ImGui::TextUnformatted("场景/模型/测试.json");
            ImGui::Button("Save scene");
            const Render::FrameExtent sceneExtent{256,128};
            const auto output = renderer.prepareSceneTarget(sceneExtent);
            ImGui::Image(static_cast<ImTextureID>(renderer.sceneImage(output)),{256,128});
            ImGui::End();
            renderer.imgui().endFrame();
            auto* data = renderer.imgui().drawData();
            renderer.drawSceneTarget(output,{sceneExtent, {}, {}}, {0.12f, 0.16f, 0.22f, 1},
                               [&](void* commands) { renderer.imgui().recordDrawData(data, commands); });
        }

        std::ostringstream messages_;
        std::filesystem::path previousPath_;
    };

    TEST_F(ImGuiIntegrationTest, DpiRoundTripRebuildsOnlyOnChangeAndRendersCommonChinese)
    {
        auto native = window(true);
        glfwSetWindowPos(static_cast<GLFWwindow*>(native->getNativeHandle()), 40, 40);
        DpiWindow controlled(*native);
        controlled.scale = 1;
        const auto output = std::filesystem::current_path().parent_path() / "review/task01-imgui";
        std::filesystem::create_directories(output);
        {
            Render::Renderer renderer(controlled, Render::RendererValidationMode::Required);
            EXPECT_TRUE(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable);
            EXPECT_FALSE(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable);
            int width, height, pixelWidth, pixelHeight;
            native->getWindowSize(width, height);
            native->getFramebufferSize(pixelWidth, pixelHeight);
            const float density = float(pixelHeight) / height;
            for (const float scale : {1.0f, 1.5f, 2.0f, 1.0f})
            {
                SCOPED_TRACE(scale);
                controlled.scale = scale;
                native->beginInputFrame();
                native->pollEvents();
                render(renderer);
                auto& io = ImGui::GetIO();
                EXPECT_FLOAT_EQ(io.FontDefault->FontSize, 18 * scale);
                EXPECT_FLOAT_EQ(io.FontGlobalScale, 1 / density);
                EXPECT_FLOAT_EQ(ImGui::GetStyle().WindowPadding.x, std::floor(12 * scale / density));
                EXPECT_FLOAT_EQ(ImGui::GetStyle().FramePadding.y, std::floor(6 * scale / density));
                EXPECT_EQ(ImGui::GetPlatformIO().Viewports.Size, 1);
                for (const ImWchar character : {ImWchar(0x573a), ImWchar(0x666f), ImWchar(0x6a21),
                                                ImWchar(0x578b), ImWchar(0x6d4b), ImWchar(0x8bd5)})
                    EXPECT_NE(io.FontDefault->FindGlyphNoFallback(character), nullptr);
                const auto texture = io.Fonts->TexID;
                const auto log = messages_.str();
                for (int frame = 0; frame < 3; ++frame)
                    render(renderer);
                EXPECT_EQ(io.Fonts->TexID, texture);
                EXPECT_EQ(messages_.str(), log);
                WindowCapture::save(WindowCapture::capture(*native),
                                    output / ("simulated-" + std::to_string(int(scale * 100)) + ".ppm"));
            }
        }
        EXPECT_EQ(messages_.str().find("error|"), std::string::npos) << messages_.str();
    }

    TEST_F(ImGuiIntegrationTest, FramebufferDensityKeepsLogicalSizesAndRasterizesAtPhysicalSize)
    {
        auto native = window();
        DpiWindow controlled(*native);
        controlled.scale = 2;
        controlled.density = 2;
        Render::Integration::ImGuiRenderer integration(controlled);
        int width, height, pixelWidth, pixelHeight;
        native->getWindowSize(width, height);
        native->getFramebufferSize(pixelWidth, pixelHeight);
        const float density = 2 * float(pixelHeight) / height;
        EXPECT_FLOAT_EQ(ImGui::GetIO().FontDefault->FontSize, 36);
        EXPECT_FLOAT_EQ(ImGui::GetIO().FontGlobalScale, 1 / density);
        EXPECT_FLOAT_EQ(ImGui::GetStyle().WindowPadding.x, std::floor(24 / density));
    }

    TEST_F(ImGuiIntegrationTest, NativeDpiRendersWithSingleWindowAndValidFontTexture)
    {
        auto native = window(true);
        glfwSetWindowPos(static_cast<GLFWwindow*>(native->getNativeHandle()), 40, 40);
        {
            Render::Renderer renderer(*native, Render::RendererValidationMode::Required);
            for (int frame = 0; frame < 5; ++frame)
            {
                native->beginInputFrame();
                native->pollEvents();
                render(renderer);
            }
            EXPECT_EQ(ImGui::GetPlatformIO().Viewports.Size, 1);
            EXPECT_NE(ImGui::GetIO().Fonts->TexID, ImTextureID{});
            const auto output = std::filesystem::current_path().parent_path() / "review/task01-imgui";
            std::filesystem::create_directories(output);
            WindowCapture::save(WindowCapture::capture(*native), output / "native-dpi.ppm");
            std::ofstream(output / "native-dpi.txt") << "Native content scale: " << native->getContentScale()
                                                   << "\nFont raster size: " << ImGui::GetIO().FontDefault->FontSize
                                                   << "\nFontGlobalScale: " << ImGui::GetIO().FontGlobalScale << '\n';
        }
        EXPECT_EQ(messages_.str().find("error|"), std::string::npos) << messages_.str();
    }
}  // namespace
