#include <gtest/gtest.h>
#include <imgui.h>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <glm/ext/matrix_transform.hpp>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "examples/demo/demo_app.h"
#include "examples/demo/model_scene.h"
#include "examples/demo/startup_config.h"
#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/model/prepared_model.h"
#include "runtime/render/integration/imgui_renderer.h"
#include "runtime/resource/loader/text_resource.h"
#include "runtime/resource/loader/texture_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"
#include "runtime/scene/scene_frame.h"
#include "support/texture_gpu_review.h"
#include "support/window_capture.h"
#include "test/examples/demo/showcase_scene.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource;
    namespace fs = std::filesystem;
    namespace NativeIO = Incubator::FileIO;
    constexpr const char* GLTF = "models/minimal_gltf/model.gltf";
    constexpr const char* OBJ = "models/minimal_obj/model.obj";
    constexpr const char* PNG = "textures/direction_rgba.png";
    constexpr const char* DEMO_GLTF = "models/avocado/glTF/Avocado.gltf";
    constexpr const char* DEMO_OBJ = "models/utah_teapot/utah_teapot.obj";
    constexpr const char* DEMO_PNG = "models/avocado/glTF/Avocado_baseColor.png";

    struct Lifetime
    {
        bool ioDestroyed = false;
        bool routesEmptyAtIoClose = false;
        bool gpuAliveAtIoClose = false;
        bool windowAliveAtIoClose = false;
        bool callbackReleased = true;
        bool callbackReleasedAtIoClose = false;
        bool modelAliveAtIoClose = false;
        size_t opens = 0;
        HWND window = nullptr;
        Render::Mesh mesh;
        std::weak_ptr<const Model::PreparedModel> model;
    };

    class TrackedFileIO final : public NativeIO::FileIO
    {
    public:
        explicit TrackedFileIO(std::shared_ptr<Lifetime> state)
            : state_(std::move(state)), native_(NativeIO::createPlatformFileIO())
        {}
        ~TrackedFileIO() override
        {
            state_->ioDestroyed = true;
            state_->routesEmptyAtIoClose = ResourceManager::instance().loadersEmpty();
            state_->gpuAliveAtIoClose = state_->mesh.valid();
            state_->windowAliveAtIoClose = state_->window && IsWindow(state_->window);
            state_->callbackReleasedAtIoClose = state_->callbackReleased;
            state_->modelAliveAtIoClose = !state_->model.expired();
        }
        bool exists(const fs::path& path) override
        {
            return native_->exists(path);
        }
        uint64_t getFileSize(const fs::path& path) override
        {
            return native_->getFileSize(path);
        }
        bool remove(const fs::path& path) override
        {
            return native_->remove(path);
        }
        std::unique_ptr<NativeIO::FileHandle> open(const fs::path& path, NativeIO::OpenMode mode,
                                                   NativeIO::ShareMode share) override
        {
            ++state_->opens;
            return native_->open(path, mode, share);
        }
        bool directoryExists(const fs::path& path) override
        {
            return native_->directoryExists(path);
        }
        bool createDirectory(const fs::path& path) override
        {
            return native_->createDirectory(path);
        }
        bool removeDirectory(const fs::path& path) override
        {
            return native_->removeDirectory(path);
        }

    private:
        std::shared_ptr<Lifetime> state_;
        std::unique_ptr<NativeIO::FileIO> native_;
    };

    class OriginalResource final : public ResourceObject
    {
    public:
        OriginalResource(ResourceId id, ResourceType type) : ResourceObject(id, type) {}
    };

    class OriginalLoader final : public LoaderBase
    {
    public:
        OriginalLoader(ResourceType type, std::shared_ptr<bool> destroyed)
            : type_(type), destroyed_(std::move(destroyed))
        {}
        ~OriginalLoader() override
        {
            *destroyed_ = true;
        }
        std::unique_ptr<ResourceObject> load(const ResourceId& id, std::string_view) override
        {
            if (type_ == ResourceType::TEXT)
                return std::make_unique<TextResource>(id, "original route");
            return std::make_unique<OriginalResource>(id, type_);
        }

    private:
        ResourceType type_;
        std::shared_ptr<bool> destroyed_;
    };

    class DemoAppTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            previous_ = fs::current_path();
            sandbox_ =
                previous_ / "review/application" / ::testing::UnitTest::GetInstance()->current_test_info()->name();
            fs::remove_all(sandbox_);
            fs::create_directories(sandbox_ / "assets");
            const auto fixtureRoot = Utils::Utf8::toFsPath(RUNTIME_TEST_ASSET_DIR);
            for (const char* directory : {"models", "textures", "shaders", "fonts"})
                fs::copy(fixtureRoot / directory, sandbox_ / "assets" / directory,
                         fs::copy_options::recursive);
            fs::current_path(sandbox_);
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            Log::Registry::reset();
            auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(renderLog_);
            sink->set_pattern("%l|%v");
            for (const char* name : {"render", "render.vulkan"})
            {
                Log::Registry::get(name).addSink(sink);
                Log::Registry::get(name).setLevel(Log::Level::DEBUG);
            }
        }
        void TearDown() override
        {
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            Log::Registry::reset();
            fs::current_path(previous_);
        }
        void copyShowcaseModels()
        {
            for (const char* directory : {"avocado", "utah_teapot"})
                fs::copy(previous_ / "assets/models" / directory, sandbox_ / "assets/models" / directory,
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            fs::create_directories(sandbox_ / "assets/scenes");
            fs::copy_file(previous_ / "assets/scenes/mixed.json", sandbox_ / "assets/scenes/mixed.json",
                          fs::copy_options::overwrite_existing);
        }
        Demo::DemoOptions options(bool visible = false)
        {
            Demo::DemoOptions result;
            result.window.title = "Incubator application session";
            result.window.width = 640;
            result.window.height = 480;
            result.window.visible = visible;
            result.validation = Render::RendererValidationMode::Required;
            result.clearColor = {0.2f, 0.24f, 0.28f, 1};
            return result;
        }
        void track(Demo::DemoApp& session, const std::shared_ptr<const Model::PreparedModel>& model,
                   const std::shared_ptr<Lifetime>& state)
        {
            state->window = glfwGetWin32Window(static_cast<GLFWwindow*>(session.window().getNativeHandle()));
            state->model = model;
            state->mesh = model->meshes()[0];
        }
        void closed(const std::shared_ptr<Lifetime>& state)
        {
            EXPECT_TRUE(state->ioDestroyed);
            EXPECT_TRUE(state->routesEmptyAtIoClose);
            EXPECT_FALSE(state->gpuAliveAtIoClose);
            EXPECT_FALSE(state->windowAliveAtIoClose);
            EXPECT_TRUE(state->callbackReleasedAtIoClose);
            EXPECT_FALSE(state->modelAliveAtIoClose);
            EXPECT_TRUE(ResourceManager::instance().loadersEmpty());
        }
        void unloaded(ResourceType type, const char* path)
        {
            auto& registry = ResourceRegistry::instance();
            const auto id = registry.lookup(type, path);
            ASSERT_TRUE(id.valid());
            EXPECT_EQ(registry.refCount(id), 0u);
            EXPECT_EQ(registry.getObject(id), nullptr);
        }
        void routes()
        {
            const auto& manager = ResourceManager::instance();
            EXPECT_TRUE(manager.hasLoader(ResourceType::SHADER));
            EXPECT_TRUE(manager.hasLoader(ResourceType::TEXTURE));
            EXPECT_TRUE(manager.hasLoader(ResourceType::MODEL));
        }
        void cleanValidation()
        {
            const auto text = renderLog_.str();
            std::cout << text;
            EXPECT_NE(text.find("validation: enabled"), std::string::npos);
            EXPECT_EQ(text.find("warning|"), std::string::npos);
            EXPECT_EQ(text.find("error|"), std::string::npos);
        }
        size_t count(std::string_view value)
        {
            const auto text = renderLog_.str();
            size_t result = 0;
            for (auto pos = text.find(value); pos != std::string::npos; pos = text.find(value, pos + value.size()))
                ++result;
            return result;
        }
        fs::path previous_, sandbox_;
        std::ostringstream renderLog_;
    };

    TEST_F(DemoAppTest, FileAssociationIgnoresPendingPathAndFailedActions)
    {
        copyShowcaseModels();
        Demo::DemoApp session(options());
        session.run({[&](auto& app) {
                         Demo::ModelScene demo;
                         demo.initialize(app);
                         const std::string original(demo.scenePath());
                         demo.operationPath("unconfirmed.json");
                         ASSERT_TRUE(demo.save(app));
                         EXPECT_EQ(demo.scenePath(), original);
                         EXPECT_FALSE(fs::exists("unconfirmed.json"));
                         demo.beginSaveAs();
                         demo.operationPath("cancelled.json");
                         demo.cancelFileAction();
                         EXPECT_FALSE(demo.confirmFileAction(app));
                         EXPECT_EQ(demo.scenePath(), original);
                         demo.beginSaveAs();
                         demo.operationPath("missing-dir/file.json");
                         EXPECT_FALSE(demo.confirmFileAction(app));
                         EXPECT_EQ(demo.scenePath(), original);
                         demo.operationPath("saved-copy.json");
                         ASSERT_TRUE(demo.confirmFileAction(app));
                         EXPECT_EQ(demo.scenePath(), "saved-copy.json");
                         demo.beginOpen();
                         demo.operationPath("missing.json");
                         EXPECT_FALSE(demo.confirmFileAction(app));
                         EXPECT_EQ(demo.scenePath(), "saved-copy.json");
                         demo.operationPath(original);
                         ASSERT_TRUE(demo.confirmFileAction(app));
                         EXPECT_EQ(demo.scenePath(), original);
                         demo.clear(app);
                         EXPECT_EQ(demo.scenePath(), original);
                         app.requestExit();
                     },
                     {},
                     {}});
        cleanValidation();
    }

    TEST_F(DemoAppTest, WorkspacePersistsDockingAndResetKeepsScene)
    {
        copyShowcaseModels();
        for (int pass = 0; pass < 2; ++pass)
        {
            auto config = options(true);
            config.window.width = pass ? 1280 : 1920;
            config.window.height = pass ? 720 : 1080;
            Demo::DemoApp session(config);
            auto demo = std::make_shared<Demo::ModelScene>();
            int frame = 0;
            session.run({[demo](auto& app) {
                             demo->initialize(app);
                             demo->select(app.scene().entities()[0].id);
                         },
                         [&](auto& app, const auto&) {
                             if (++frame == 6)
                             {
                                 const auto image = WindowCapture::capture(app.window());
                                 WindowCapture::save(
                                     image, pass ? "review/workspace-restored.ppm" : "review/workspace-default.ppm");
                             }
                             if (frame == 8)
                                 app.requestExit();
                         },
                         [&, demo](auto& app) {
                             demo->ui(app);
                             for (const char* name : {"Objects", "Properties", "Scene"})
                             {
                                 const auto* panel = ImGui::FindWindowByName(name);
                                 ASSERT_NE(panel, nullptr);
                                 EXPECT_NE(panel->DockId, 0u) << name;
                             }
                             EXPECT_EQ(ImGui::FindWindowByName("Files"), nullptr);
                             EXPECT_EQ(ImGui::FindWindowByName("Status"), nullptr);
                             if (frame >= 5)
                             {
                                 const auto size = ImGui::GetMainViewport()->Size;
                                 EXPECT_GT(app.viewportRegion().size.y, size.y * 0.7f);
                                 EXPECT_GT(app.viewportRegion().size.x, size.x * 0.4f);
                             }
                             if (frame == 3)
                             {
                                 const auto count = app.scene().entities().size();
                                 demo->restoreLayout();
                                 EXPECT_EQ(app.scene().entities().size(), count);
                                 ImGui::SaveIniSettingsToDisk("incubator-layout.ini");
                             }
                             glfwWaitEventsTimeout(0.04);
                         },
                         {},
                         {},
                         [demo](auto& app) { demo->layout(app); }});
            EXPECT_TRUE(fs::exists("incubator-layout.ini"));
        }
        cleanValidation();
    }

    TEST_F(DemoAppTest, StartupConfigOpensCustomSceneAndTracksSuccessfulFileOperations)
    {
        auto io = NativeIO::createPlatformFileIO();
        Scene::SceneDescription content;
        content.entities.push_back({Model::ModelReference(GLTF), Scene::Transform({1, 2, 0}), false});
        content.entities.push_back({std::nullopt, Scene::Transform({4, 5, 6}), true});
        content.camera = Scene::Camera({0, 1, 9});
        content.lighting = {{0, -2, -3}, {0.2f, 0.6f, 0.8f}, 2, 0.3f};
        content.clearColor = {0.2f, 0.3f, 0.4f, 1};
        Scene::writeScene(*io, "custom-scene.json", content);
        {
            std::ofstream configFile("incubator.json");
            configFile << R"({"window":{"title":"配置窗口","width":640,"height":480},"startupScene":"custom-scene.json"})";
        }
        auto config = Demo::readStartupConfig(*io);
        config.options.window.visible = false;
        config.options.validation = Render::RendererValidationMode::Required;
        Demo::DemoApp session(config.options, std::move(io));
        session.run({[&](auto& app) {
            Demo::ModelScene demo;
            demo.initialize(app, config.startupScene);
            EXPECT_EQ(demo.scenePath(), "custom-scene.json");
            EXPECT_FALSE(demo.selected().valid());
            ASSERT_EQ(app.scene().entities().size(), 2u);
            EXPECT_EQ(app.scene().entities()[0].transform.position(), glm::vec3(1, 2, 0));
            EXPECT_FALSE(app.scene().entities()[0].visible);
            EXPECT_EQ(app.scene().entities()[1].model, nullptr);
            EXPECT_EQ(app.scene().camera().position(), content.camera.position());
            EXPECT_EQ(app.scene().lighting(), content.lighting);
            EXPECT_EQ(app.clearColor(), content.clearColor);
            int width, height;
            app.window().getWindowSize(width, height);
            EXPECT_EQ(width, 640);
            EXPECT_EQ(height, 480);

            EXPECT_FALSE(ResourceRegistry::instance().lookup(ResourceType::MODEL, DEMO_GLTF).valid());
            EXPECT_FALSE(ResourceRegistry::instance().lookup(ResourceType::MODEL, DEMO_OBJ).valid());
            demo.select(app.scene().entities()[0].id);
            EXPECT_FALSE(demo.load(app, "missing-scene.json"));
            EXPECT_EQ(demo.scenePath(), "custom-scene.json");
            EXPECT_TRUE(demo.selected().valid());
            ASSERT_TRUE(demo.save(app, "另存场景.json"));
            EXPECT_EQ(demo.scenePath(), "另存场景.json");
            ASSERT_TRUE(demo.save(app, demo.scenePath()));
            EXPECT_FALSE(demo.save(app, "missing-directory/scene.json"));
            EXPECT_EQ(demo.scenePath(), "另存场景.json");
            ASSERT_TRUE(demo.load(app, "custom-scene.json"));
            EXPECT_EQ(demo.scenePath(), "custom-scene.json");
            EXPECT_FALSE(demo.selected().valid());
            app.requestExit();
        }, {}, {}});
        EXPECT_FALSE(session.isOpen());
        cleanValidation();
    }

    TEST_F(DemoAppTest, StartupSceneErrorsStopBeforeUpdateAndCloseSession)
    {
        auto io = NativeIO::createPlatformFileIO();
        Scene::SceneDescription description;
        description.entities.push_back({Model::ModelReference("models/missing.gltf"), Scene::Transform{}, true});
        Scene::writeScene(*io, "missing-model-scene.json", description);
        std::ofstream("invalid-scene.json") << "{}";
        for (const char* path : {"missing-scene.json", "invalid-scene.json", "missing-model-scene.json"})
        {
            SCOPED_TRACE(path);
            auto state = std::make_shared<Lifetime>();
            Demo::DemoApp session(options(), std::make_unique<TrackedFileIO>(state));
            auto callbacks = Demo::makeCallbacks(path);
            int updates = 0;
            callbacks.update = [&](auto&, const auto&) { ++updates; };
            EXPECT_THROW(session.run(std::move(callbacks)), Error::Exception);
            EXPECT_EQ(updates, 0);
            EXPECT_FALSE(session.isOpen());
            closed(state);
        }
        cleanValidation();
    }

    TEST_F(DemoAppTest, SceneViewportUsesCurrentLayoutAndSurvivesHiddenFrames)
    {
        Demo::DemoApp session(options());
        int frame = 0;
        bool open = true;
        Render::FrameExtent first{};
        session.run({[&](auto& app) {
                         const auto model = Model::prepareModel(app.renderer(), GLTF);
                         app.scene().createEntity(model);
                         app.scene().setCamera(Scene::Camera({0, 0, 3}));
                     },
                     [&](auto& app, const auto&) {
                         if (++frame == 8)
                             app.requestExit();
                     },
                     [&](auto& app) {
                         if (frame == 1)
                             first = app.sceneExtent();
                         if (frame == 2)
                             EXPECT_NE(app.sceneExtent(), first);
                         if (frame == 3 || frame == 4)
                             EXPECT_EQ(app.sceneExtent(), Render::FrameExtent{});
                         if (frame >= 5)
                             EXPECT_GT(app.sceneExtent().width, 0u);
                         const auto entity = app.scene().entities()[0].id;
                         app.scene().setTransform(entity, Scene::Transform({float(frame) * 0.01f, 0, 0}));
                         ImGui::Begin("Other UI");
                         ImGui::Text("Frame %d", frame);
                         ImGui::End();
                     },
                     {},
                     {},
                     [&](auto& app) {
                         open = frame != 3;
                         if (frame > 1)
                             ImGui::SetWindowSize("Viewport", {float(400 + frame * 10), 300});
                         ImGui::SetWindowCollapsed("Viewport", frame == 4);
                         app.sceneViewport("Viewport", &open);
                     }});
        EXPECT_EQ(frame, 8);
        cleanValidation();
    }

    TEST_F(DemoAppTest, OrbitFocusCommandsKeepAssetsAndEntitiesAndSynchronizeReplacement)
    {
        copyShowcaseModels();
        auto state = std::make_shared<Lifetime>();
        Demo::DemoApp session(options(), std::make_unique<TrackedFileIO>(state));
        session.run({[&](Demo::DemoApp& app) {
            Demo::ModelScene demo;
            demo.initialize(app);
            DemoTest::setSharedModelScene(app, demo, false);
            const auto id = demo.selected();
            const auto transform = app.scene().find(id)->transform.matrix();
            const auto opens = state->opens;
            const auto camera = app.scene().camera().position();
            Demo::UiInput input;
            input.focusPressed = true;
            input.captureKeyboard = true;
            demo.input(app, {800, 600}, input);
            EXPECT_EQ(app.scene().camera().position(), camera);
            demo.select({});
            EXPECT_FALSE(demo.focusSelected(app, {800, 600}));
            demo.select(app.scene().createEntity());
            EXPECT_FALSE(demo.focusSelected(app, {800, 600}));
            demo.select(id);
            input.captureKeyboard = false;
            demo.input(app, {800, 600}, input);
            EXPECT_NE(app.scene().camera().position(), camera);
            EXPECT_EQ(app.scene().find(id)->transform.matrix(), transform);
            demo.resetCamera(app);
            EXPECT_EQ(app.scene().camera().position(), glm::vec3(0, 0, 6));
            app.scene().setCamera(Scene::Camera({0, 0, 8}));
            EXPECT_EQ(app.scene().camera().position(), glm::vec3(0, 0, 8));
            demo.input(app, {800, 600}, {});
            EXPECT_EQ(app.scene().camera().position(), glm::vec3(0, 0, 8));
            DemoTest::setSharedModelScene(app, demo, false);
            demo.input(app, {800, 600}, {});
            EXPECT_EQ(app.scene().camera().position(), glm::vec3(0, 0, 6));
            EXPECT_EQ(app.scene().find(id), nullptr);
            EXPECT_EQ(state->opens, opens);
            app.requestExit();
        }, {}, {}});
        cleanValidation();
    }

    TEST_F(DemoAppTest, WindowInputChainsWithImGuiAndDelayedBatchesSurviveRawFrameReset)
    {
        auto native = Window::createPlatformWindow({"Input integration", 640, 480, true, false});
        auto* handle = static_cast<GLFWwindow*>(native->getNativeHandle());
        const auto focus = glfwSetWindowFocusCallback(handle, nullptr);
        glfwSetWindowFocusCallback(handle, focus);
        focus(handle, GLFW_TRUE);
        {
            Render::Integration::ImGuiRenderer ui(*native);
            const auto mouse = glfwSetMouseButtonCallback(handle, nullptr);
            glfwSetMouseButtonCallback(handle, mouse);
            const auto position = glfwSetCursorPosCallback(handle, nullptr);
            glfwSetCursorPosCallback(handle, position);
            const auto key = glfwSetKeyCallback(handle, nullptr);
            glfwSetKeyCallback(handle, key);
            const auto scroll = glfwSetScrollCallback(handle, nullptr);
            glfwSetScrollCallback(handle, scroll);
            const auto character = glfwSetCharCallback(handle, nullptr);
            glfwSetCharCallback(handle, character);
            const auto uiFocus = glfwSetWindowFocusCallback(handle, nullptr);
            glfwSetWindowFocusCallback(handle, uiFocus);
            auto& io = ImGui::GetIO();
            io.DisplaySize = {640, 480};
            io.DeltaTime = 1.0f / 60;
            unsigned char* pixels;
            int width, height;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            Demo::UiInputAdapter adapter;
            int characters = 0;
            int focusPresses = 0;
            Input::PointerValue wheel;
            bool focusText = false;
            char text[32]{};
            const auto sample = [&] {
                ImGui::NewFrame();
                characters += io.InputQueueCharacters.Size;
                const auto result = adapter.sample(native->input());
                focusPresses += result.focusPressed;
                wheel.x += result.scroll.x;
                wheel.y += result.scroll.y;
                ImGui::SetNextWindowPos({0, 0});
                ImGui::SetNextWindowSize({200, 200});
                ImGui::Begin("Panel");
                ImGui::TextUnformatted("Input panel");
                if (focusText) ImGui::SetKeyboardFocusHere();
                ImGui::InputText("Text", text, sizeof(text));
                ImGui::End();
                ImGui::EndFrame();
                return result;
            };
            EXPECT_TRUE(io.ConfigInputTrickleEventQueue);
            position(handle, 100, 100);
            sample();
            sample();
            mouse(handle, 0, GLFW_PRESS, 0);
            sample();
            native->beginInputFrame();
            mouse(handle, 0, GLFW_RELEASE, 0);
            position(handle, 300, 300);
            mouse(handle, 1, GLFW_PRESS, 0);
            EXPECT_TRUE(native->input().mouseButton(Input::MouseButton::Right).pressed);
            EXPECT_FALSE(sample().rotate.pressed);
            native->beginInputFrame();
            EXPECT_FALSE(native->input().mouseButton(Input::MouseButton::Right).pressed);
            const auto delayed = sample();
            EXPECT_TRUE(delayed.rotate.pressed);
            EXPECT_FALSE(delayed.captureMouse);
            Demo::OrbitCamera orbit;
            Scene::Camera camera({0, 0, 6});
            EXPECT_FALSE(orbit.apply(camera, delayed, {640, 480}));
            position(handle, 320, 300);
            EXPECT_TRUE(orbit.apply(camera, sample(), {640, 480}));
            const auto dragged = camera.position();
            mouse(handle, 1, GLFW_RELEASE, 0);
            mouse(handle, 1, GLFW_PRESS, 0);
            mouse(handle, 1, GLFW_RELEASE, 0);
            EXPECT_TRUE(sample().rotate.released);
            native->beginInputFrame();
            EXPECT_TRUE(sample().rotate.pressed);
            EXPECT_TRUE(sample().rotate.released);
            key(handle, GLFW_KEY_F, 0, GLFW_PRESS, 0);
            scroll(handle, 1, 2);
            character(handle, 'x');
            for (int i = 0; i < 4; ++i) sample();
            EXPECT_EQ(characters, 1);
            EXPECT_EQ(focusPresses, 1);
            EXPECT_EQ(wheel, (Input::PointerValue{1, 2}));
            EXPECT_TRUE(native->input().key(Input::KeyCode::F).pressed);
            EXPECT_EQ(native->input().scroll(), (Input::PointerValue{1, 2}));
            native->pollEvents();
            native->pollEvents();
            EXPECT_EQ(native->input().scroll(), (Input::PointerValue{1, 2}));
            mouse(handle, 1, GLFW_PRESS, 0);
            mouse(handle, 1, GLFW_RELEASE, 0);
            key(handle, GLFW_KEY_F, 0, GLFW_RELEASE, 0);
            adapter.cancel();
            orbit.cancel();
            uiFocus(handle, GLFW_FALSE);
            uiFocus(handle, GLFW_TRUE);
            EXPECT_TRUE(native->input().focusLost());
            for (int i = 0; i < 5; ++i)
            {
                const auto canceled = sample();
                EXPECT_FALSE(canceled.rotate.pressed);
                EXPECT_FALSE(orbit.apply(camera, canceled, {640, 480}));
            }
            EXPECT_EQ(camera.position(), dragged);
            mouse(handle, 1, GLFW_PRESS, 0);
            EXPECT_TRUE(sample().rotate.pressed);
            mouse(handle, 1, GLFW_RELEASE, 0);
            position(handle, 100, 100);
            sample();
            sample();
            scroll(handle, 0, 1);
            EXPECT_TRUE(sample().captureMouse);
            focusText = true;
            sample();
            focusText = false;
            sample();
            key(handle, GLFW_KEY_F, 0, GLFW_PRESS, 0);
            const auto typing = sample();
            EXPECT_TRUE(typing.captureKeyboard);
            EXPECT_TRUE(typing.focusPressed);
        }
        native->beginInputFrame();
        focus(handle, GLFW_TRUE);
        const auto key = glfwSetKeyCallback(handle, nullptr);
        glfwSetKeyCallback(handle, key);
        key(handle, GLFW_KEY_A, 0, GLFW_PRESS, 0);
        EXPECT_TRUE(native->input().key(Input::KeyCode::A).pressed);
    }

    TEST_F(DemoAppTest, AnyExistingRouteRejectsTakeoverWithoutChangingOriginalLoader)
    {
        for (const auto type : {ResourceType::TEXT, static_cast<ResourceType>(0x12345678)})
        {
            auto& manager = ResourceManager::instance();
            auto destroyed = std::make_shared<bool>(false);
            manager.registerLoader(type, std::make_unique<OriginalLoader>(type, destroyed));
            auto state = std::make_shared<Lifetime>();
            try
            {
                Demo::DemoApp session(options(), std::make_unique<TrackedFileIO>(state));
                FAIL() << "Accepted nonempty route table";
            }
            catch (const Error::Exception& error)
            {
                EXPECT_EQ(error.code(), Error::Code::CONFIG);
            }
            EXPECT_TRUE(state->ioDestroyed);
            EXPECT_FALSE(state->routesEmptyAtIoClose);
            EXPECT_FALSE(*destroyed);
            EXPECT_TRUE(manager.hasLoader(type));
            EXPECT_FALSE(manager.hasLoader(ResourceType::SHADER));
            EXPECT_FALSE(manager.hasLoader(ResourceType::TEXTURE));
            EXPECT_FALSE(manager.hasLoader(ResourceType::MODEL));
            const auto id = manager.registerResource(type, "original.txt");
            const auto* object = manager.load(id);
            ASSERT_NE(object, nullptr);
            EXPECT_EQ(object->type(), type);
            if (type == ResourceType::TEXT)
                EXPECT_EQ(dynamic_cast<const TextResource*>(object)->text(), "original route");
            manager.release(id);
            manager.reset();
            EXPECT_TRUE(*destroyed);
        }
        EXPECT_EQ(count("Created Vulkan"), 0u);
    }

    TEST_F(DemoAppTest, ShaderInitializationFailureReleasesHoldsAndOwnedRoutesBeforeFileIO)
    {
        auto state = std::make_shared<Lifetime>();
        auto config = options();
        config.vertexShader = "shaders/missing.vert.spv";
        try
        {
            Demo::DemoApp session(config, std::make_unique<TrackedFileIO>(state));
            FAIL() << "Accepted missing Shader";
        }
        catch (const Error::Exception& error)
        {
            EXPECT_EQ(error.code(), Error::Code::RENDER);
        }
        closed(state);
        unloaded(ResourceType::SHADER, "shaders/missing.vert.spv");
        unloaded(ResourceType::SHADER, "shaders/cube.frag.spv");
        EXPECT_EQ(count("Created Vulkan"), 0u);
    }

    TEST_F(DemoAppTest, LiveRoutesAllowLaterModelPreparationWithOtherCpuOwner)
    {
        auto state = std::make_shared<Lifetime>();
        Demo::DemoApp session(options(true), std::make_unique<TrackedFileIO>(state));
        ResourceId otherPng;
        const TextureResource* otherPixels = nullptr;
        size_t ordinaryOpens = 0;
        session.run(
            {[&](Demo::DemoApp& app) {
                 routes();
                 unloaded(ResourceType::SHADER, "shaders/cube.vert.spv");
                 unloaded(ResourceType::SHADER, "shaders/cube.frag.spv");
                 const auto model = Model::prepareModel(app.renderer(), GLTF);
                 track(app, model, state);
                 app.scene().createEntity(model);
                 unloaded(ResourceType::MODEL, GLTF);
                 unloaded(ResourceType::TEXTURE, PNG);
                 otherPng = ResourceRegistry::instance().lookup(ResourceType::TEXTURE, PNG);
                 ResourceManager::instance().acquire(otherPng);
                 otherPixels = ResourceManager::instance().get<TextureResource>(otherPng);
                 ASSERT_NE(otherPixels, nullptr);
                 ordinaryOpens = state->opens;
             },
             [&](Demo::DemoApp& app, const Demo::FrameTime&) {
                 routes();
                 EXPECT_EQ(state->opens, ordinaryOpens);
                 const auto model = Model::prepareModel(app.renderer(), OBJ);
                 app.scene().createEntity(model);
                 EXPECT_EQ(ResourceRegistry::instance().refCount(otherPng), 1u);
                 EXPECT_EQ(ResourceRegistry::instance().getObject(otherPng), otherPixels);
                 track(app, model, state);
                 unloaded(ResourceType::MODEL, OBJ);
                 app.requestExit();
             },
             {}});
        EXPECT_FALSE(session.isOpen());
        closed(state);
        EXPECT_EQ(ResourceRegistry::instance().refCount(otherPng), 1u);
        EXPECT_EQ(ResourceManager::instance().get<TextureResource>(otherPng), otherPixels);
        ResourceManager::instance().release(otherPng);
        unloaded(ResourceType::TEXTURE, PNG);
        cleanValidation();
    }

    TEST_F(DemoAppTest, MinimizedLoopsAdvanceTimeAndUpdateWhileSkippingUiThenRestoreNewExtent)
    {
        auto state = std::make_shared<Lifetime>();
        Demo::DemoApp session(options(true), std::make_unique<TrackedFileIO>(state));
        std::vector<Demo::FrameTime> times;
        std::vector<int> uiUpdates;
        int updates = 0;
        size_t opens = 0;
        int cancellations = 0;
        session.run({[&](Demo::DemoApp& app) {
                         const auto model = Model::prepareModel(app.renderer(), GLTF);
                         track(app, model, state);
                         app.scene().setCamera(Scene::Camera({0, 0, 1}, {1, 0, 0, 0}, {glm::radians(90.0f), 0.1f, 10}));
                         app.scene().createEntity(model, Scene::Transform({0, 0, 0}, {1, 0, 0, 0}, glm::vec3(0.5f)));
                         opens = state->opens;
                     },
                     [&](Demo::DemoApp& app, const Demo::FrameTime& time) {
                         const int frame = updates++;
                         times.push_back(time);
                         EXPECT_GE(time.delta, Clock::Duration::zero());
                         if (times.size() > 1)
                             EXPECT_EQ(time.delta, time.now - times[times.size() - 2].now);
                         app.scene().createEntity();  // 跳过渲染时逻辑仍会继续执行。
                         EXPECT_EQ(state->opens, opens);
                         auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         if (frame == 1)
                         {
                             glfwIconifyWindow(handle);
                             for (int attempt = 0; attempt < 100 && !app.window().isIconified(); ++attempt)
                             {
                                 app.window().pollEvents();
                                 glfwWaitEventsTimeout(0.005);
                             }
                             ASSERT_TRUE(app.window().isIconified());
                         }
                         if (frame == 5)
                         {
                             EXPECT_EQ(uiUpdates, (std::vector<int>{0}));
                             EXPECT_EQ(app.scene().entities().size(), 7u);
                             glfwRestoreWindow(handle);
                             for (int attempt = 0; attempt < 100 && app.window().isIconified(); ++attempt)
                             {
                                 app.window().pollEvents();
                                 glfwWaitEventsTimeout(0.005);
                             }
                             ASSERT_FALSE(app.window().isIconified());
                             glfwSetWindowSize(handle, 800, 400);
                         }
                         if (frame == 10)
                         {
                             int width, height;
                             app.window().getFramebufferSize(width, height);
                             EXPECT_EQ(width, 800);
                             EXPECT_EQ(height, 400);
                             const auto restored = Scene::extractSceneFrame(
                                 app.scene(), {static_cast<uint32_t>(width), static_cast<uint32_t>(height)});
                             ASSERT_TRUE(restored);
                             EXPECT_FALSE(restored->draws.empty());
                             app.requestExit();
                         }
                     },
                     [&](Demo::DemoApp& app) {
                         uiUpdates.push_back(updates - 1);
                         EXPECT_FALSE(app.window().isIconified());
                         glfwWaitEventsTimeout(0.04);
                     },
                     [&](Demo::DemoApp&) { ++cancellations; }});
        EXPECT_EQ(updates, 11);
        EXPECT_GE(cancellations, 4);
        EXPECT_EQ(uiUpdates, (std::vector<int>{0, 5, 6, 7, 8, 9}));
        closed(state);
        cleanValidation();
    }

    TEST_F(DemoAppTest, LightingWidgetsEditCurrentSceneAndSameFrameSnapshot)
    {
        auto state = std::make_shared<Lifetime>();
        auto config = options(true);
        config.window.height = 1000;
        Demo::DemoApp session(config, std::make_unique<TrackedFileIO>(state));
        const auto opens = state->opens;
        const Render::Lighting initial{{1, 2, -3}, {0.5f, 0.75f, 1}, 0.7f, 0.2f};
        const Render::Lighting external{{2, 1, -3}, {0.2f, 0.4f, 0.8f}, 0.4f, 0.3f};
        const Render::Lighting replacement{{-2, -3, -4}, {0.8f, 0.1f, 1}, 0.5f, 0.4f};
        ImVec2 intensityClick, yawClick;
        int frames = 0;
        Scene::SceneFrame retained;
        session.run({[&](Demo::DemoApp& app) {
                         app.scene().setLighting(initial);
                         auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         glfwSetCursorPosCallback(handle, nullptr);
                         const auto enter = glfwSetCursorEnterCallback(handle, nullptr);
                         ASSERT_NE(enter, nullptr);
                         enter(handle, GLFW_TRUE);
                         glfwFocusWindow(handle);
                     },
                     [&](Demo::DemoApp& app, const Demo::FrameTime&) {
                         auto& io = ImGui::GetIO();
                         if (frames == 3 || frames == 5)
                         {
                             const auto position = frames == 3 ? intensityClick : yawClick;
                             io.AddMousePosEvent(position.x, position.y);
                             io.AddMouseButtonEvent(0, true);
                         }
                         if (frames == 4 || frames == 6)
                             io.AddMouseButtonEvent(0, false);
                         if (frames == 7)
                             app.scene().setLighting(external);
                         if (frames == 8)
                         {
                             auto scene = std::make_unique<Scene::Scene>();
                             scene->setLighting(replacement);
                             app.setScene(std::move(scene));
                         }
                         if (frames == 9)
                             app.requestExit();
                     },
                     [&](Demo::DemoApp& app) {
                         ImGui::SetNextWindowPos({12, 12}, ImGuiCond_Always);
                         // 面板需容纳 DPI 缩放后的五组标签和控件，避免把点击发到裁剪区。
                         const auto row = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing();
                         const auto height = 5 * row + ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().WindowPadding.y;
                         ImGui::SetNextWindowSize({600, height}, ImGuiCond_Always);
                         ImGui::Begin("Lighting controls");
                         ImGui::PushItemWidth(200);
                         const auto origin = ImGui::GetCursorScreenPos();
                         yawClick = {origin.x + 50,
                                     origin.y + ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeight() * 0.5f};
                         intensityClick = {origin.x + 150, yawClick.y + 3 * row};
                         Demo::lightingControls(app.scene());
                         ImGui::PopItemWidth();
                         ImGui::End();
                         const auto frame = Scene::extractSceneFrame(app.scene(), {800, 600});
                         ASSERT_TRUE(frame);
                         EXPECT_EQ(frame->input().lighting, app.scene().lighting());
                         if (frames == 3)
                         {
                             EXPECT_GT(frame->lighting.intensity, 2.0f);
                             EXPECT_EQ(frame->lighting.direction, initial.direction);
                             retained = *frame;
                         }
                         if (frames == 5)
                         {
                             EXPECT_NE(frame->lighting.direction, initial.direction);
                             EXPECT_NEAR(glm::length(frame->lighting.direction), 1, 0.00001f);
                             EXPECT_EQ(frame->lighting.intensity, retained.lighting.intensity);
                             EXPECT_TRUE(frame->lighting.valid());
                         }
                         if (frames == 7)
                             EXPECT_EQ(frame->lighting, external);
                         if (frames == 8)
                             EXPECT_EQ(frame->lighting, replacement);
                         EXPECT_EQ(state->opens, opens);
                         ++frames;
                     }});
        EXPECT_EQ(frames, 9);
        EXPECT_GT(retained.lighting.intensity, 2.0f);
        EXPECT_EQ(retained.lighting.direction, initial.direction);
        EXPECT_EQ(count("Uploaded mesh:"), 0u);
        EXPECT_EQ(count("Created material:"), 0u);
        closed(state);
        cleanValidation();
    }

    TEST_F(DemoAppTest, QueuedPanAndCanceledRotateUseConsumedInputBatches)
    {
        auto native = Window::createPlatformWindow({"Consumed input batches", 640, 480, false, false});
        auto* handle = static_cast<GLFWwindow*>(native->getNativeHandle());
        Render::Integration::ImGuiRenderer ui(*native);
        const auto mouse = glfwSetMouseButtonCallback(handle, nullptr);
        glfwSetMouseButtonCallback(handle, mouse);
        const auto position = glfwSetCursorPosCallback(handle, nullptr);
        glfwSetCursorPosCallback(handle, position);
        const auto focus = glfwSetWindowFocusCallback(handle, nullptr);
        glfwSetWindowFocusCallback(handle, focus);
        focus(handle, GLFW_TRUE);
        auto& io = ImGui::GetIO();
        io.DisplaySize = {640, 480};
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        Demo::UiInputAdapter adapter;
        Demo::OrbitCamera orbit;
        Scene::Camera camera({0, 0, 6});
        const auto sample = [&] {
            native->beginInputFrame();
            ImGui::NewFrame();
            const auto input = adapter.sample(native->input());
            orbit.apply(camera, input, {640, 480});
            ImGui::EndFrame();
            return input;
        };
        ASSERT_TRUE(io.ConfigInputTrickleEventQueue);
        position(handle, 300, 300);
        sample();
        mouse(handle, 2, GLFW_PRESS, 0);
        mouse(handle, 2, GLFW_RELEASE, 0);
        position(handle, 320, 300);
        EXPECT_TRUE(sample().pan.pressed);
        EXPECT_TRUE(sample().pan.released);
        EXPECT_EQ(camera.position(), glm::vec3(0, 0, 6));
        // 按下、释放与位移分属三批；不能按 update 轮数假定位移已经属于按住的手势。
        EXPECT_EQ(sample().delta.x, 20);
        EXPECT_EQ(camera.position(), glm::vec3(0, 0, 6));
        mouse(handle, 2, GLFW_PRESS, 0);
        ASSERT_TRUE(sample().pan.pressed);
        position(handle, 340, 300);
        EXPECT_EQ(sample().delta.x, 20);
        EXPECT_NE(camera.position(), glm::vec3(0, 0, 6));
        mouse(handle, 2, GLFW_RELEASE, 0);
        ASSERT_TRUE(sample().pan.released);

        const auto canceled = camera;
        adapter.cancel();
        orbit.cancel();
        focus(handle, GLFW_FALSE);
        focus(handle, GLFW_TRUE);
        // 四个边沿刻意排队，复现恢复时旧事件尚未消费的情况。
        for (int i = 0; i < 2; ++i)
        {
            mouse(handle, 1, GLFW_PRESS, 0);
            mouse(handle, 1, GLFW_RELEASE, 0);
        }
        for (int i = 0; i < 4; ++i)
        {
            const auto input = sample();
            EXPECT_FALSE(input.rotate.pressed);
            EXPECT_EQ(camera.position(), canceled.position());
            EXPECT_EQ(camera.rotation(), canceled.rotation());
        }
        ASSERT_TRUE(ImGui::GetCurrentContext()->InputEventsQueue.empty());
        EXPECT_FALSE(sample().rotate.down);  // 中性帧不重新启动已取消的手势。
        mouse(handle, 1, GLFW_PRESS, 0);
        ASSERT_TRUE(sample().rotate.pressed);
        position(handle, 360, 310);
        EXPECT_EQ(sample().delta.x, 20);
        EXPECT_NE(camera.rotation(), canceled.rotation());
        mouse(handle, 1, GLFW_RELEASE, 0);
        EXPECT_TRUE(sample().rotate.released);
    }

    TEST_F(DemoAppTest, ViewportDragCrossesPanelsAndReplacementRequiresFreshPress)
    {
        Demo::DemoApp session(options());
        Demo::ModelScene demo;
        int frame = -1;
        Demo::ViewportRegion region;
        GLFWmousebuttonfun mouse = nullptr;
        GLFWcursorposfun cursor = nullptr;
        Scene::Camera replaced;
        glm::quat moved;
        session.run({[&](auto& app) {
                         app.scene().setCamera(Scene::Camera({0, 0, 6}));
                         auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         mouse = glfwSetMouseButtonCallback(handle, nullptr);
                         glfwSetMouseButtonCallback(handle, mouse);
                         cursor = glfwSetCursorPosCallback(handle, nullptr);
                         glfwSetCursorPosCallback(handle, cursor);
                         auto focus = glfwSetWindowFocusCallback(handle, nullptr);
                         glfwSetWindowFocusCallback(handle, focus);
                         focus(handle, GLFW_TRUE);
                     },
                     [&](auto& app, const auto&) {
                         auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         switch (++frame)
                         {
                             case 1:
                                 cursor(handle, region.origin.x + 40, region.origin.y + 40);
                                 break;
                             case 2:
                                 mouse(handle, 1, GLFW_PRESS, 0);
                                 break;
                             case 3:
                                 cursor(handle, region.origin.x + 60, region.origin.y + 40);
                                 break;
                             case 4:
                                 EXPECT_NE(app.scene().camera().rotation(), glm::quat(1, 0, 0, 0));
                                 moved = app.scene().camera().rotation();
                                 cursor(handle, region.origin.x - 30, region.origin.y + 40);
                                 break;
                             case 5:
                                 EXPECT_NE(app.scene().camera().rotation(), moved);
                                 break;
                             case 6: {
                                 auto scene = std::make_unique<Scene::Scene>();
                                 scene->setCamera(Scene::Camera({0, 0, 8}));
                                 app.setScene(std::move(scene));
                                 replaced = app.scene().camera();
                                 break;
                             }
                             case 7:
                                 cursor(handle, region.origin.x + 40, region.origin.y + 40);
                                 break;
                             case 8:
                                 cursor(handle, region.origin.x + 80, region.origin.y + 50);
                                 break;
                             case 9:
                                 EXPECT_EQ(app.scene().camera().rotation(), replaced.rotation());
                                 mouse(handle, 1, GLFW_RELEASE, 0);
                                 break;
                             case 12:
                                 mouse(handle, 1, GLFW_PRESS, 0);
                                 break;
                             case 13:
                                 cursor(handle, region.origin.x + 100, region.origin.y + 50);
                                 break;
                             case 14:
                                 EXPECT_NE(app.scene().camera().rotation(), replaced.rotation());
                                 mouse(handle, 1, GLFW_RELEASE, 0);
                                 break;
                             case 15:
                                 app.requestExit();
                                 break;
                         }
                     },
                     {},
                     [&](auto&) { demo.cancelInteraction(); },
                     [&](auto& app, auto extent, const auto& input) { demo.input(app, extent, input); },
                     [&](auto& app) {
                         app.sceneViewport("Scene");
                         region = app.viewportRegion();
                     }});
        EXPECT_EQ(frame, 15);
        cleanValidation();
    }

    TEST_F(DemoAppTest, RunForwardsPanAndRestartsRotationAfterFocusCancellation)
    {
        Demo::DemoApp session(options());
        Demo::ModelScene demo;
        GLFWmousebuttonfun mouse = nullptr;
        GLFWcursorposfun position = nullptr;
        GLFWwindowfocusfun focus = nullptr;
        int frame = -1, cancellations = 0;
        Scene::Camera canceled;
        session.run({[&](auto& app) {
                         app.scene().setCamera(Scene::Camera({0, 0, 6}));
                         auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         mouse = glfwSetMouseButtonCallback(handle, nullptr);
                         glfwSetMouseButtonCallback(handle, mouse);
                         position = glfwSetCursorPosCallback(handle, nullptr);
                         glfwSetCursorPosCallback(handle, position);
                         focus = glfwSetWindowFocusCallback(handle, nullptr);
                         glfwSetWindowFocusCallback(handle, focus);
                         focus(handle, GLFW_TRUE);
                     },
                     [&](auto& app, const auto&) {
                         auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         switch (++frame)
                         {
                             case 0:
                                 position(handle, 500, 300);
                                 break;
                             case 1:
                                 mouse(handle, 2, GLFW_PRESS, 0);
                                 break;
                             case 2:
                                 position(handle, 520, 300);
                                 break;
                             case 3:
                                 EXPECT_NE(app.scene().camera().position(), glm::vec3(0, 0, 6));
                                 mouse(handle, 2, GLFW_RELEASE, 0);
                                 break;
                             case 4:
                                 canceled = app.scene().camera();
                                 focus(handle, GLFW_FALSE);
                                 break;
                             case 5:
                                 focus(handle, GLFW_TRUE);
                                 break;
                             case 6:
                                 // 焦点丢失会清空 ImGui 鼠标位置，先恢复基准，再开始新的拖动。
                                 position(handle, 520, 300);
                                 EXPECT_EQ(app.scene().camera().position(), canceled.position());
                                 EXPECT_EQ(app.scene().camera().rotation(), canceled.rotation());
                                 break;
                             case 7:
                                 mouse(handle, 1, GLFW_PRESS, 0);
                                 break;
                             case 8:
                                 position(handle, 540, 310);
                                 break;
                             case 9:
                                 EXPECT_NE(app.scene().camera().rotation(), canceled.rotation());
                                 mouse(handle, 1, GLFW_RELEASE, 0);
                                 break;
                             case 10:
                                 app.requestExit();
                                 break;
                         }
                     },
                     {},
                     [&](auto&) {
                         ++cancellations;
                         demo.cancelInteraction();
                     },
                     [&](auto& app, auto extent, const auto& input) {
                         EXPECT_NE(frame, 4);
                         EXPECT_TRUE(ImGui::GetCurrentContext()->InputEventsQueue.empty());
                         if (frame == 6)
                             EXPECT_EQ(input.position, (Input::PointerValue{520, 300}));
                         if (frame == 1)
                             EXPECT_TRUE(input.pan.pressed);
                         if (frame == 7)
                             EXPECT_TRUE(input.rotate.pressed);
                         if (frame == 8)
                             EXPECT_EQ(input.delta, (Input::PointerValue{20, 10}));
                         demo.input(app, extent, input);
                     }});
        EXPECT_EQ(cancellations, 1);
        cleanValidation();
    }

    TEST_F(DemoAppTest, BothShowcaseModelsPresentSharedInstances)
    {
        copyShowcaseModels();
        auto config = options(true);
        config.window.width = 800;
        config.window.height = 600;
        auto state = std::make_shared<Lifetime>();
        Demo::DemoApp session(config, std::make_unique<TrackedFileIO>(state));
        auto demo = std::make_shared<Demo::ModelScene>();
        int frames = 0;
        std::shared_ptr<const Model::PreparedModel> gltf;
        const auto presented = [&](Demo::DemoApp& app) {
            const auto image = WindowCapture::capture(app.window());
            const auto bounds = WindowCapture::bounds(image, 370);
            EXPECT_GT(bounds[2], bounds[0]);
            EXPECT_GT(bounds[3], bounds[1]);
            const auto entities = app.scene().entities();
            ASSERT_EQ(entities.size(), 2u);
            EXPECT_NE(entities[0].model, nullptr);
            EXPECT_EQ(entities[0].model, entities[1].model);
        };
        Demo::Callbacks callbacks{[&, demo](auto& app) {
                                      demo->initialize(app);
                                      DemoTest::setSharedModelScene(app, *demo, false);
                                      gltf = app.scene().entities()[0].model;
                                  },
                                  [&, demo](auto& app, const auto&) {
                                      if (frames == 2)
                                      {
                                          ASSERT_NO_FATAL_FAILURE(presented(app));
                                          DemoTest::setSharedModelScene(app, *demo, true);
                                          EXPECT_NE(app.scene().entities()[0].model, gltf);
                                          track(app, app.scene().entities()[0].model, state);
                                      }
                                      if (frames++ == 4)
                                      {
                                          ASSERT_NO_FATAL_FAILURE(presented(app));
                                          app.requestExit();
                                      }
                                  },
                                  [&, demo](auto& app) {
                                      demo->ui(app);
                                      glfwWaitEventsTimeout(0.04);
                                  }};
        demo.reset();
        session.run(std::move(callbacks));
        closed(state);
        cleanValidation();
    }

    TEST_F(DemoAppTest, SceneCommandsAndUiChangesReachCurrentFrame)
    {
        copyShowcaseModels();
        auto state = std::make_shared<Lifetime>();
        Demo::DemoApp session(options(), std::make_unique<TrackedFileIO>(state));
        auto demo = std::make_shared<Demo::ModelScene>();
        const std::weak_ptr<Demo::ModelScene> lifetime = demo;
        int updates = 0;
        Demo::Callbacks callbacks{[demo](auto& app) {
                                      demo->initialize(app);
                                      DemoTest::setSharedModelScene(app, *demo, false);
                                  },
                                  [&, demo](auto& app, const auto&) {
                                      if (++updates == 3)
                                          app.requestExit();
                                  },
                                  [&, demo](auto& app) {
                                      demo->ui(app);
                                      if (updates != 2)
                                          return;
                                      const auto first = demo->selected();
                                      const auto second = app.scene().entities()[1].id;
                                      const auto gltf = app.scene().find(first)->model;
                                      const auto opens = state->opens;
                                      // 这是 UI 回调内的方法接入检查；实际控件输入另由 LightingWidgets 用例验证。
                                      ASSERT_TRUE(app.scene().setVisible(first, false));
                                      const auto frame = Scene::extractSceneFrame(app.scene(), {640, 480});
                                      ASSERT_TRUE(frame);
                                      EXPECT_EQ(frame->draws.size(), gltf->draws().size());
                                      const auto created = demo->createEntity(app);
                                      EXPECT_EQ(demo->selected(), created);
                                      EXPECT_EQ(app.scene().find(created)->model, gltf);
                                      demo->deleteSelected(app);
                                      EXPECT_FALSE(demo->selected().valid());
                                      EXPECT_EQ(app.scene().find(created), nullptr);
                                      EXPECT_NE(app.scene().find(second), nullptr);
                                      const auto lighting = app.scene().lighting();
                                      const auto camera = app.scene().camera();
                                      demo->clear(app);
                                      EXPECT_TRUE(app.scene().entities().empty());
                                      EXPECT_FALSE(demo->selected().valid());
                                      EXPECT_EQ(app.scene().lighting(), lighting);
                                      EXPECT_EQ(app.scene().camera().position(), camera.position());
                                      demo->createEntity(app);
                                      app.scene().setCamera(Scene::Camera({0, 0, 8}));
                                      EXPECT_EQ(app.scene().camera().position().z, 8);
                                      DemoTest::setSharedModelScene(app, *demo, true);
                                      const auto obj = app.scene().find(demo->selected())->model;
                                      EXPECT_NE(obj, gltf);
                                      EXPECT_EQ(app.scene().find(first), nullptr);
                                      EXPECT_EQ(app.scene().camera().position().z, 6);
                                      DemoTest::setSharedModelScene(app, *demo, false);
                                      EXPECT_EQ(app.scene().find(demo->selected())->model, gltf);
                                      EXPECT_EQ(state->opens, opens);
                                      track(app, gltf, state);
                                  }};
        demo.reset();
        session.run(std::move(callbacks));
        EXPECT_TRUE(lifetime.expired());
        closed(state);
        cleanValidation();
    }

    TEST_F(DemoAppTest, MixedSceneSaveLoadPreservesStateAndRenderedPixels)
    {
        copyShowcaseModels();
        Demo::DemoApp session(options());
        session.run({[&](Demo::DemoApp& app) {
            Demo::ModelScene demo;
            demo.initialize(app);
            DemoTest::setSharedModelScene(app, demo, false);

            const auto original = app.scene().entities()[0].id;
            const auto avocado = app.scene().find(original)->model;
            demo.selectModel(true);
            const auto tea = demo.createEntity(app);
            const auto teapot = app.scene().find(tea)->model;
            ASSERT_NE(avocado, teapot);
            const Scene::Transform pose({1.6f, 0, 0}, {1, 0, 0, 0}, {0.075f, 0.075f, 0.075f});
            ASSERT_TRUE(app.scene().setTransform(tea, pose));
            app.scene().createEntity({}, Scene::Transform({5, 1, 2}, {1, 0, 0, 1}, {-1, 0, 2}), false);
            app.scene().setLighting({{0.2f, -1, -2}, {0.7f, 0.8f, 1}, 1.2f, 0.2f});
            app.clearColor() = {0.1f, 0.2f, 0.3f, 1};
            const auto camera = app.scene().camera();
            auto before = Scene::extractSceneFrame(app.scene(), {384, 256});
            ASSERT_TRUE(before);
            const auto pixels = TextureGpuReview::captureFrame(app.renderer(), before->input());
            const auto uploads = count("Uploaded mesh:");
            ASSERT_TRUE(demo.save(app, "mixed-save.json")) << demo.message();
            EXPECT_FALSE(demo.load(app, "missing-scene.json"));
            EXPECT_NE(app.scene().find(tea), nullptr);
            EXPECT_EQ(demo.selected(), tea);
            EXPECT_EQ(app.clearColor(), (std::array<float, 4>{0.1f, 0.2f, 0.3f, 1}));
            auto invalidModel = Scene::captureScene(app.scene(), app.clearColor());
            invalidModel.entities[2].model = Model::ModelReference("models/missing.gltf");
            Scene::writeScene(app.fileIO(), "missing-model.json", invalidModel);
            EXPECT_FALSE(demo.load(app, "missing-model.json"));
            EXPECT_NE(app.scene().find(tea), nullptr);
            EXPECT_EQ(demo.selected(), tea);
            EXPECT_EQ(app.scene().camera().position(), camera.position());
            ASSERT_TRUE(demo.load(app, "mixed-save.json")) << demo.message();
            EXPECT_FALSE(demo.selected().valid());
            EXPECT_EQ(app.scene().find(original), nullptr);
            ASSERT_EQ(app.scene().entities().size(), 4u);
            EXPECT_EQ(app.scene().entities()[0].model, avocado);
            EXPECT_EQ(app.scene().entities()[2].model, teapot);
            EXPECT_FALSE(app.scene().entities()[3].visible);
            EXPECT_EQ(app.scene().entities()[3].model, nullptr);
            EXPECT_EQ(app.scene().entities()[2].transform.scale(), pose.scale());
            EXPECT_EQ(app.scene().camera().position(), camera.position());
            auto after = Scene::extractSceneFrame(app.scene(), {384, 256});
            ASSERT_TRUE(after);
            const auto restored = TextureGpuReview::captureFrame(app.renderer(), after->input());
            ASSERT_EQ(pixels.size(), restored.size());
            for (size_t i = 0; i < pixels.size(); ++i)
                EXPECT_LE(std::abs(int(pixels[i]) - int(restored[i])), 2) << i;
            const auto saveImage = [](const char* path, const std::vector<uint8_t>& rgba) {
                std::ofstream output(path, std::ios::binary);
                output << "P6\n384 256\n255\n";
                for (size_t i = 0; i < rgba.size(); i += 4)
                    output.write(reinterpret_cast<const char*>(rgba.data() + i), 3);
                return bool(output);
            };
            ASSERT_TRUE(saveImage("scene-before.ppm", pixels));
            ASSERT_TRUE(saveImage("scene-restored.ppm", restored));
            EXPECT_EQ(count("Uploaded mesh:"), uploads);
            ASSERT_TRUE(demo.load(app, "assets/scenes/mixed.json")) << demo.message();
            ASSERT_EQ(app.scene().entities().size(), 2u);
            EXPECT_EQ(app.scene().entities()[0].model, avocado);
            EXPECT_EQ(app.scene().entities()[1].model, teapot);
            demo.selectModel(false);
            EXPECT_EQ(app.scene().find(demo.createEntity(app))->model, avocado);
            demo.clear(app);
            ASSERT_TRUE(demo.save(app, "empty-save.json"));
            ASSERT_TRUE(demo.load(app, "empty-save.json"));
            EXPECT_TRUE(app.scene().entities().empty());
            app.requestExit();
        }, {}, {}});
        cleanValidation();
    }

    TEST_F(DemoAppTest, SceneFileCommandsObserveSameUiFrame)
    {
        copyShowcaseModels();
        Demo::DemoApp session(options());
        auto demo = std::make_shared<Demo::ModelScene>();
        session.run({[demo](auto& app) {
            demo->initialize(app);
            DemoTest::setSharedModelScene(app, *demo, false);
        }, {}, [demo](auto& app) {
            demo->ui(app);
            const auto old = demo->selected();
            const Scene::Transform edited({2, 1, 0}, {1, 0, 0, 0}, {-2, 0, 3});
            ASSERT_TRUE(app.scene().setTransform(old, edited));
            ASSERT_TRUE(demo->save(app, "same-frame.json"));
            app.scene().setTransform(old, Scene::Transform{});
            ASSERT_TRUE(demo->load(app, "same-frame.json"));
            EXPECT_EQ(app.scene().find(old), nullptr);
            EXPECT_FALSE(demo->selected().valid());
            ASSERT_EQ(app.scene().entities().size(), 2u);
            EXPECT_EQ(app.scene().entities()[0].transform.matrix(), edited.matrix());
            const auto frame = Scene::extractSceneFrame(app.scene(), {640, 480}, false);
            ASSERT_TRUE(frame);
            EXPECT_EQ(frame->draws[0].model, edited.matrix() * app.scene().entities()[0].model->draws()[0].model);
            app.requestExit();
        }});
        demo.reset();
        cleanValidation();
    }

    struct CallbackHold
    {
        explicit CallbackHold(std::shared_ptr<Lifetime> state) : state(std::move(state))
        {
            this->state->callbackReleased = false;
        }
        ~CallbackHold()
        {
            if (cpu.valid())
                ResourceManager::instance().release(cpu);
            EXPECT_TRUE(ResourceManager::instance().hasLoader(ResourceType::MODEL));
            EXPECT_TRUE(state->mesh.valid());
            state->callbackReleased = true;
        }
        std::shared_ptr<Lifetime> state;
        ResourceId cpu;
    };

    TEST_F(DemoAppTest, InitializeUpdateAndUiExceptionsReleaseCallbacksAndResourcesBeforeFixtureCleanup)
    {
        for (int phase = 0; phase < 3; ++phase)
        {
            SCOPED_TRACE(phase);
            auto state = std::make_shared<Lifetime>();
            Demo::DemoApp session(options(), std::make_unique<TrackedFileIO>(state));
            auto hold = std::make_shared<CallbackHold>(state);
            int updates = 0, uis = 0;
            Demo::Callbacks callbacks{
                [&, hold](Demo::DemoApp& app) {
                    auto model = Model::prepareModel(app.renderer(), GLTF);
                    track(app, model, state);
                    app.scene().createEntity(model);
                    hold->cpu = ResourceRegistry::instance().lookup(ResourceType::TEXTURE, PNG);
                    ResourceManager::instance().acquire(hold->cpu);
                    EXPECT_NE(ResourceManager::instance().get<TextureResource>(hold->cpu), nullptr);
                    if (phase == 0)
                        throw std::runtime_error("initialize failure");
                },
                [&, hold](Demo::DemoApp&, const Demo::FrameTime&) {
                    ++updates;
                    if (phase == 1)
                        throw std::runtime_error("update failure");
                },
                [&, hold](Demo::DemoApp&) {
                    ++uis;
                    throw std::runtime_error("ui failure");
                }};
            hold.reset();
            EXPECT_THROW(session.run(std::move(callbacks)), std::runtime_error);
            EXPECT_FALSE(session.isOpen());
            EXPECT_EQ(updates, phase == 0 ? 0 : 1);
            EXPECT_EQ(uis, phase == 2 ? 1 : 0);
            closed(state);
            unloaded(ResourceType::MODEL, GLTF);
            unloaded(ResourceType::TEXTURE, PNG);
            unloaded(ResourceType::SHADER, "shaders/cube.vert.spv");
            unloaded(ResourceType::SHADER, "shaders/cube.frag.spv");
        }
        cleanValidation();
    }

    TEST_F(DemoAppTest, MissingModelPngStopsDemoAppAndReleasesDependencyHolds)
    {
        fs::remove("assets/textures/direction_rgba.png");
        auto state = std::make_shared<Lifetime>();
        Demo::DemoApp session(options(), std::make_unique<TrackedFileIO>(state));
        int updates = 0;
        EXPECT_THROW(session.run({[&](Demo::DemoApp& app) {
                                      Model::prepareModel(app.renderer(), GLTF);
                                      app.requestExit();
                                  },
                                  [&](Demo::DemoApp&, const Demo::FrameTime&) { ++updates; },
                                  {}}),
                     Error::Exception);
        EXPECT_EQ(updates, 0);
        EXPECT_FALSE(session.isOpen());
        closed(state);
        unloaded(ResourceType::MODEL, GLTF);
        unloaded(ResourceType::TEXTURE, PNG);
        EXPECT_EQ(count("Uploaded mesh:"), 0u);
        cleanValidation();
    }

    TEST_F(DemoAppTest, ExitWindowCloseAndDestructionStopFurtherCallbacks)
    {
        for (int mode = 0; mode < 4; ++mode)
        {
            SCOPED_TRACE(mode);
            auto state = std::make_shared<Lifetime>();
            int updates = 0, uis = 0;
            {
                Demo::DemoApp session(options(), std::make_unique<TrackedFileIO>(state));
                auto model = Model::prepareModel(session.renderer(), GLTF);
                track(session, model, state);
                session.scene().createEntity(model);
                model.reset();
                if (mode < 3)
                    session.run({{},
                                 [&](Demo::DemoApp& app, const Demo::FrameTime&) {
                                     if (++updates == 3 && mode < 2)
                                     {
                                         if (mode == 0)
                                             app.requestExit();
                                         else
                                             app.window().setShouldClose(true);
                                     }
                                 },
                                 [&](Demo::DemoApp& app) {
                                     ++uis;
                                     if (mode == 2)
                                         app.requestExit();
                                 }});
                else
                    session.requestExit();  // 未调用 run 就销毁对象也是正常的关闭路径。
            }
            EXPECT_EQ(updates, mode < 2 ? 3 : mode == 2 ? 1 : 0);
            EXPECT_EQ(uis, mode < 2 ? 2 : mode == 2 ? 1 : 0);
            closed(state);
        }
        cleanValidation();
    }

}  // namespace
