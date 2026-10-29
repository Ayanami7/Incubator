#include <GLFW/glfw3.h>
#include <gtest/gtest.h>
#include <imgui_internal.h>
#include <spdlog/sinks/ostream_sink.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "examples/demo/showcase.h"
#include "runtime/core/log/logger.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"
#include "support/window_capture.h"

namespace
{
    using namespace Incubator;
    namespace fs = std::filesystem;

    class DemoShowcaseTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            previous_ = fs::current_path();
            const auto sandbox =
                previous_ / "review/demo-showcase" / ::testing::UnitTest::GetInstance()->current_test_info()->name();
            fs::remove_all(sandbox);
            fs::create_directories(sandbox);
            fs::copy(previous_ / "assets", sandbox / "assets", fs::copy_options::recursive);
            fs::current_path(sandbox);
            auto& manager = Resource::ResourceManager::instance();
            manager.reset();
            Resource::ResourceRegistry::instance().clear();
            manager.bindAssetSource(nullptr);
            Log::Registry::reset();
            auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(messages_);
            sink->set_pattern("%l|%v");
            for (const auto* name : {"render", "render.vulkan"})
                Log::Registry::get(name).addSink(sink);
        }
        void TearDown() override
        {
            EXPECT_FALSE(Resource::ResourceRegistry::instance().hasLiveResources());
            EXPECT_TRUE(Resource::ResourceManager::instance().loadersEmpty());
            EXPECT_EQ(Resource::ResourceManager::instance().assetSource(), nullptr);
            EXPECT_NE(messages_.str().find("validation: enabled"), std::string::npos);
            EXPECT_EQ(messages_.str().find("warning|"), std::string::npos) << messages_.str();
            EXPECT_EQ(messages_.str().find("error|"), std::string::npos) << messages_.str();
            Log::Registry::reset();
            Resource::ResourceRegistry::instance().clear();
            fs::current_path(previous_);
        }
        App::SessionOptions options(bool visible = false)
        {
            App::SessionOptions result;
            result.window.width = 800;
            result.window.height = 600;
            result.window.visible = visible;
            result.validation = Render::RendererValidationMode::Required;
            return result;
        }
        fs::path previous_;
        std::ostringstream messages_;
    };

    TEST_F(DemoShowcaseTest, StaticSceneCameraLightingAndShadowControlsKeepModelSharing)
    {
        Demo::DemoApp app(options(true));
        auto showcase = std::make_shared<Demo::Showcase>();
        auto callbacks = Demo::makeCallbacks();
        callbacks.initialize = [showcase](auto& session) { showcase->initialize(session); };
        callbacks.layout = [showcase](auto& session) { showcase->layout(session); };
        callbacks.input = [showcase](auto& session, auto extent, const auto& input) {
            showcase->input(session, extent, input);
        };
        callbacks.cancelInteraction = [showcase](auto&) { showcase->cancelInteraction(); };
        int updates = 0, uiFrames = 0;
        std::vector<std::shared_ptr<const Model::PreparedModel>> models;
        callbacks.update = [&](auto& session, const auto&) {
            if (++updates == 7)
                session.requestExit();
        };
        callbacks.ui = [&](auto& session) {
            ++uiFrames;
            const auto entities = session.scene().entities();
            ASSERT_EQ(entities.size(), 3u);
            if (models.empty())
                for (const auto& entity : entities)
                    models.push_back(entity.model);
            const auto camera = session.scene().camera();
            App::UiInput input;
            input.scroll.y = 1;
            showcase->input(session, session.sceneExtent(), input);
            EXPECT_NE(session.scene().camera().position(), camera.position());
            showcase->resetCamera(session);
            EXPECT_EQ(session.scene().camera().position(), camera.position());
            auto light = session.scene().lighting();
            light.intensity = 0.75f;
            session.scene().setLighting(light);
            session.shadowSettings().enabled = uiFrames % 2;
            session.shadowSettings().depthBias = 0.002f;
            if (uiFrames == 3)
            {
                auto* controls = ImGui::FindWindowByName("Demo controls");
                ASSERT_NE(controls, nullptr);
                controls->StateStorage.SetInt(controls->GetID("Shadow bias"), 1);
            }
            showcase->ui(session);
            EXPECT_EQ(ImGui::FindWindowByName("Objects"), nullptr);
            EXPECT_EQ(ImGui::FindWindowByName("Properties"), nullptr);
            EXPECT_FLOAT_EQ(session.scene().lighting().intensity, 0.75f);
            for (size_t i = 0; i < models.size(); ++i)
                EXPECT_EQ(entities[i].model, models[i]);
            if (uiFrames == 5)
            {
                glfwWaitEventsTimeout(0.03);
                WindowCapture::save(WindowCapture::capture(session.window()), "review/demo-minimal.ppm");
            }
        };
        app.run(std::move(callbacks));
        EXPECT_GE(uiFrames, 5);
        for (const auto& model : models)
            EXPECT_FALSE(model->meshes()[0].valid());
    }

    TEST_F(DemoShowcaseTest, ViewportRestoresAfterMinimizeAndDoesNotWriteEditorLayout)
    {
        {
            std::ofstream editorLayout("incubator-editor-layout.ini");
            editorLayout << "editor layout sentinel";
        }
        Demo::DemoApp app(options(true));
        auto callbacks = Demo::makeCallbacks();
        int updates = 0, uiFrames = 0, canceled = 0;
        callbacks.update = [&](auto& session, const auto&) {
            auto* window = static_cast<GLFWwindow*>(session.window().getNativeHandle());
            ++updates;
            if (updates == 2)
                glfwIconifyWindow(window);
            if (updates == 5)
            {
                EXPECT_TRUE(session.window().isIconified());
                glfwRestoreWindow(window);
                glfwSetWindowSize(window, 960, 540);
            }
            if (updates == 10)
            {
                EXPECT_FALSE(session.window().isIconified());
                EXPECT_GT(session.sceneExtent().width, 800u);
                session.requestExit();
            }
        };
        const auto ui = callbacks.ui;
        callbacks.ui = [&](auto& session) {
            ++uiFrames;
            EXPECT_EQ(ImGui::GetIO().IniFilename, nullptr);
            ui(session);
            glfwWaitEventsTimeout(0.02);
        };
        const auto cancel = callbacks.cancelInteraction;
        callbacks.cancelInteraction = [&](auto& session) {
            ++canceled;
            cancel(session);
        };
        app.run(std::move(callbacks));
        EXPECT_GT(canceled, 0);
        EXPECT_GT(uiFrames, 2);
        std::ifstream input("incubator-editor-layout.ini");
        std::string saved;
        std::getline(input, saved);
        EXPECT_EQ(saved, "editor layout sentinel");
    }

    TEST_F(DemoShowcaseTest, FailedInitializationAndCallbackExceptionCloseOwnedSession)
    {
        {
            Demo::DemoApp app(options());
            EXPECT_THROW(app.run(Demo::makeCallbacks("missing-scene.json")), Error::Exception);
            EXPECT_FALSE(app.isOpen());
        }
        {
            Demo::DemoApp app(options());
            auto callbacks = Demo::makeCallbacks();
            callbacks.update = [](auto&, const auto&) { throw std::runtime_error("controlled demo interruption"); };
            EXPECT_THROW(app.run(std::move(callbacks)), std::runtime_error);
            EXPECT_FALSE(app.isOpen());
        }
    }
}  // namespace
