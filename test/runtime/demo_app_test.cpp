#include "examples/demo/demo_app.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <glm/ext/matrix_transform.hpp>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "runtime/core/utils/utf8.h"
#include "examples/demo/model_scene.h"
#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/model/prepared_model.h"
#include "runtime/resource/loader/text_resource.h"
#include "runtime/resource/loader/texture_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"
#include "runtime/scene/scene_frame.h"
#include "support/window_capture.h"
#include "runtime/render/integration/imgui_renderer.h"

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
            for (const char* directory : {"models", "textures", "shaders"})
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
        void color(const WindowCapture::Image& image, float x, float y, std::array<int, 3> rgb)
        {
            const int px = image.width / 2 + static_cast<int>(x * image.height / 2);
            const int py = image.height / 2 - static_cast<int>(y * image.height / 2);
            ASSERT_GE(px, 0);
            ASSERT_LT(px, image.width);
            ASSERT_GE(py, 0);
            ASSERT_LT(py, image.height);
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(image.rgba[(size_t(py) * image.width + px) * 4 + channel], rgb[channel], 12);
        }
        fs::path previous_, sandbox_;
        std::ostringstream renderLog_;
    };

    TEST(OrbitCameraTest, GesturesStartOnlyOnScenePressAndCancelWithoutRestartingHeldButtons)
    {
        Demo::OrbitCamera orbit;
        Scene::Camera camera({0, 0, 6});
        Demo::UiInput input;
        input.rotate = {true, true, false};
        input.delta = {100, 100};
        input.captureMouse = true;
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600}));
        input.rotate.pressed = false;
        input.captureMouse = false;
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600}));
        input.rotate.pressed = true;
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600}));
        input.rotate.pressed = false;
        input.delta = {20, 0};
        input.captureMouse = true; // Ownership survives crossing the UI.
        EXPECT_TRUE(orbit.apply(camera, input, {800, 600}));
        EXPECT_NEAR(camera.position().x, -6 * std::sin(0.1f), 0.00001f);
        EXPECT_NEAR(camera.position().z, 6 * std::cos(0.1f), 0.00001f);
        const auto moved = camera.position();
        input.rotate = {false, false, true};
        EXPECT_TRUE(orbit.apply(camera, input, {800, 600}));
        EXPECT_NE(camera.position(), moved);
        const auto released = camera.position();
        orbit.cancel();
        input.rotate = {true, false, false};
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600}));
        EXPECT_EQ(camera.position(), released);
        input.captureMouse = false;
        input.rotate = {false, true, true};
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600}));
        input.rotate = {true, true, false};
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600}));
        input.delta = {};
        input.rotate = {false, false, true};
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600}));
        EXPECT_EQ(camera.position(), released);
        camera = Scene::Camera({0, 0, 8});
        EXPECT_FALSE(orbit.apply(camera, {}, {800, 600}));
        EXPECT_EQ(camera.position(), glm::vec3(0, 0, 8));
    }

    TEST(OrbitCameraTest, PanUsesWindowCoordinatesAndZoomIsIndependentOfFrameTime)
    {
        Demo::OrbitCamera orbit;
        Scene::Camera camera({0, 0, 6}, {1, 0, 0, 0}, {glm::radians(90.0f), 0.1f, 100});
        Demo::UiInput input;
        input.pan = {true, true, false};
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600}));
        input.pan.pressed = false;
        input.delta = {30, 15};
        EXPECT_TRUE(orbit.apply(camera, input, {800, 600}));
        EXPECT_NEAR(camera.position().x, -0.6f, 0.00001f);
        EXPECT_NEAR(camera.position().y, 0.3f, 0.00001f);
        input = {};
        input.scroll.y = 1;
        EXPECT_TRUE(orbit.apply(camera, input, {800, 600}));
        EXPECT_NEAR(camera.position().z, 6 * std::exp(-0.1f), 0.00001f);
        const auto before = camera.position();
        input.captureMouse = true;
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600}));
        EXPECT_EQ(camera.position(), before);
        input = {};
        input.rotate = {true, true, false};
        orbit.apply(camera, input, {800, 600});
        input.rotate.pressed = false;
        input.delta.y = 100000;
        EXPECT_TRUE(orbit.apply(camera, input, {800, 600}));
        EXPECT_TRUE(std::isfinite(camera.position().y));
        EXPECT_GT(glm::length(camera.position()), 0);
    }

    TEST(OrbitCameraTest, FocusFramesAllWorldCornersInWideAndPortraitProjections)
    {
        const Math::Aabb geometry{{-2, -1, -3}, {4, 2, 1}};
        const auto matrix = glm::translate(glm::mat4(1), glm::vec3(10, -5, 7)) *
                            glm::mat4_cast(glm::quat(glm::vec3(0.3f, 0.8f, -0.4f))) *
                            glm::scale(glm::mat4(1), glm::vec3(-2, 3, 0.5f));
        const auto world = geometry.transformed(matrix);
        ASSERT_TRUE(world);
        for (const Render::FrameExtent extent : {Render::FrameExtent{1600, 400}, {400, 1600}})
        {
            Demo::OrbitCamera orbit;
            Scene::Camera camera({0, 0, 6}, glm::quat(glm::vec3(0.2f, 0.4f, 0)), {glm::radians(45.0f), 1, 2});
            const auto rotation = camera.rotation();
            ASSERT_TRUE(orbit.focus(camera, world, extent));
            EXPECT_EQ(camera.rotation(), rotation);
            EXPECT_FLOAT_EQ(camera.perspective().verticalFov, glm::radians(45.0f));
            const auto matrices = camera.matrices(extent);
            for (int i = 0; i < 8; ++i)
            {
                const glm::vec3 p((i & 1) ? world->max.x : world->min.x,
                                  (i & 2) ? world->max.y : world->min.y,
                                  (i & 4) ? world->max.z : world->min.z);
                const auto clip = matrices.projection * matrices.view * glm::vec4(p, 1);
                ASSERT_GT(clip.w, 0);
                EXPECT_LE(std::abs(clip.x), clip.w + 0.0001f);
                EXPECT_LE(std::abs(clip.y), clip.w + 0.0001f);
                EXPECT_GE(clip.z, -0.0001f);
                EXPECT_LE(clip.z, clip.w + 0.0001f);
            }
        }
    }

    TEST(OrbitCameraTest, UnavailableFocusPreservesGestureAndPointBoundsProduceValidCamera)
    {
        Demo::OrbitCamera orbit;
        Scene::Camera camera({0, 0, 6});
        Demo::UiInput input;
        input.rotate = {true, true, false};
        orbit.apply(camera, input, {800, 600});
        EXPECT_FALSE(orbit.focus(camera, std::nullopt, {800, 600}));
        EXPECT_EQ(camera.position(), glm::vec3(0, 0, 6));
        input.rotate.pressed = false;
        input.delta.x = 10;
        EXPECT_TRUE(orbit.apply(camera, input, {800, 600}));
        ASSERT_TRUE(orbit.focus(camera, Math::Aabb{{3, 4, 5}, {3, 4, 5}}, {800, 600}));
        EXPECT_GT(camera.perspective().nearPlane, 0);
        EXPECT_GT(camera.perspective().farPlane, camera.perspective().nearPlane);
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600})); // Focus ended the old gesture.
    }

    TEST_F(DemoAppTest, OrbitFocusCommandsKeepAssetsAndEntitiesAndSynchronizeReplacement)
    {
        copyShowcaseModels();
        auto state = std::make_shared<Lifetime>();
        Demo::DemoApp session(options(), std::make_unique<TrackedFileIO>(state));
        session.run({[&](Demo::DemoApp& app) {
            Demo::ModelScene demo;
            demo.initialize(app);
            const auto id = demo.selected();
            const auto transform = app.scene().find(id)->transform.matrix();
            const auto opens = state->opens;
            const auto uploads = count("Uploaded mesh:");
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
            demo.replaceCamera(app);
            EXPECT_EQ(app.scene().camera().position(), glm::vec3(0, 0, 8));
            demo.input(app, {800, 600}, {});
            EXPECT_EQ(app.scene().camera().position(), glm::vec3(0, 0, 8));
            demo.switchScene(app, false);
            demo.input(app, {800, 600}, {});
            EXPECT_EQ(app.scene().camera().position(), glm::vec3(0, 0, 6));
            EXPECT_EQ(app.scene().find(id), nullptr);
            EXPECT_EQ(state->opens, opens);
            EXPECT_EQ(count("Uploaded mesh:"), uploads);
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
        EXPECT_EQ(count("Uploaded mesh:"), 4u);
        EXPECT_EQ(count("Uploaded RGBA8 texture:"), 4u);
        EXPECT_EQ(count("Created material:"), 4u);
        EXPECT_EQ(count("Created graphics pipeline:"), 1u);
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
                         app.scene().createEntity();  // Logic continues even when rendering is skipped.
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
                             glfwWaitEventsTimeout(0.06);
                             const auto image = WindowCapture::capture(app.window());
                             EXPECT_EQ(image.width, 800);
                             EXPECT_EQ(image.height, 400);
                             color(image, 0.278205f, 0.235702f, {188, 255, 188});
                             WindowCapture::save(image, "review/application-restored.ppm");
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
        EXPECT_EQ(count("Uploaded mesh:"), 2u);
        EXPECT_EQ(count("Created graphics pipeline:"), 1u);
        cleanValidation();
    }

    TEST_F(DemoAppTest, BothShowcaseModelsUseProductionInputFocusAndCancelAcrossMinimize)
    {
        copyShowcaseModels();
        for (bool obj : {false, true})
        {
            SCOPED_TRACE(obj ? "Teapot" : "Avocado");
            auto state = std::make_shared<Lifetime>();
            auto config = options(true);
            config.window.width = 800;
            config.window.height = 600;
            Demo::DemoApp session(config, std::make_unique<TrackedFileIO>(state));
            Demo::ModelScene demo;
            int frame = 0, cancellations = 0;
            size_t opens = 0, uploads = 0;
            Scene::EntityId selected;
            glm::mat4 entityMatrix(1);
            Scene::Camera focused, rotated, beforeMinimize;
            GLFWmousebuttonfun button = nullptr;
            GLFWkeyfun key = nullptr;
            GLFWscrollfun scroll = nullptr;
            const auto checkFocus = [&](Demo::DemoApp& app) {
                int width, height;
                app.window().getFramebufferSize(width, height);
                const Render::FrameExtent extent{uint32_t(width), uint32_t(height)};
                const auto* entity = app.scene().find(selected);
                ASSERT_NE(entity, nullptr);
                ASSERT_TRUE(entity->model->bounds());
                const auto bounds = entity->model->bounds()->transformed(entity->transform.matrix());
                ASSERT_TRUE(bounds);
                const auto matrices = app.scene().camera().matrices(extent);
                for (int i = 0; i < 8; ++i)
                {
                    const glm::vec3 p((i & 1) ? bounds->max.x : bounds->min.x,
                                      (i & 2) ? bounds->max.y : bounds->min.y,
                                      (i & 4) ? bounds->max.z : bounds->min.z);
                    const auto clip = matrices.projection * matrices.view * glm::vec4(p, 1);
                    ASSERT_GT(clip.w, 0);
                    EXPECT_LE(std::abs(clip.x), clip.w + 0.001f);
                    EXPECT_LE(std::abs(clip.y), clip.w + 0.001f);
                    EXPECT_GE(clip.z, -0.001f);
                    EXPECT_LE(clip.z, clip.w + 0.001f);
                }
                const auto extracted = Scene::extractSceneFrame(app.scene(), extent);
                ASSERT_TRUE(extracted);
                EXPECT_GE(extracted->draws.size(), entity->model->draws().size());
                EXPECT_EQ(entity->transform.matrix(), entityMatrix);
            };
            session.run({
                [&](Demo::DemoApp& app) {
                    demo.initialize(app);
                    auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                    button = glfwSetMouseButtonCallback(handle, nullptr);
                    glfwSetMouseButtonCallback(handle, button);
                    key = glfwSetKeyCallback(handle, nullptr);
                    glfwSetKeyCallback(handle, key);
                    scroll = glfwSetScrollCallback(handle, nullptr);
                    glfwSetScrollCallback(handle, scroll);
                    glfwFocusWindow(handle);
                },
                [&](Demo::DemoApp& app, const Demo::FrameTime& time) {
                    demo.update(app, time);
                    const int step = frame++ - 4;
                    auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                    if (step == -1)
                        demo.switchScene(app, obj);
                    if (step == 0)
                    {
                        selected = demo.selected();
                        auto transform = app.scene().find(selected)->transform;
                        transform.setPosition(transform.position() + glm::vec3(100, 0, 0));
                        ASSERT_TRUE(app.scene().setTransform(selected, transform));
                        entityMatrix = transform.matrix();
                        const auto all = Scene::extractSceneFrame(app.scene(), {800, 600}, false);
                        const auto culled = Scene::extractSceneFrame(app.scene(), {800, 600});
                        EXPECT_LT(culled->draws.size(), all->draws.size());
                        opens = state->opens;
                        uploads = count("Uploaded mesh:");
                        glfwSetCursorPos(handle, 700, 500);
                        button(handle, 0, GLFW_PRESS, 0); // Defocus the panel before the shortcut.
                    }
                    if (step == 1) button(handle, 0, GLFW_RELEASE, 0);
                    if (step == 2 || step == 27) key(handle, GLFW_KEY_F, 0, GLFW_PRESS, 0);
                    if (step == 3 || step == 28) key(handle, GLFW_KEY_F, 0, GLFW_RELEASE, 0);
                    if (step == 4)
                    {
                        ASSERT_NO_FATAL_FAILURE(checkFocus(app));
                        focused = app.scene().camera();
                    }
                    if (step == 5)
                    {
                        const auto image = WindowCapture::capture(app.window());
                        size_t nonBackground = 0;
                        for (uint32_t y = 100; y < image.height - 100; ++y)
                            for (uint32_t x = 370; x < image.width - 100; ++x)
                            {
                                const auto at = (size_t(y) * image.width + x) * 4;
                                if (std::abs(int(image.rgba[at]) - 51) > 12 ||
                                    std::abs(int(image.rgba[at + 1]) - 61) > 12 ||
                                    std::abs(int(image.rgba[at + 2]) - 71) > 12)
                                    ++nonBackground;
                            }
                        EXPECT_GT(nonBackground, 0u);
                        WindowCapture::save(image, obj ? "review/input-teapot-focus.ppm" : "review/input-avocado-focus.ppm");
                        button(handle, 1, GLFW_PRESS, 0);
                    }
                    if (step == 6) glfwSetCursorPos(handle, 740, 500);
                    if (step == 7) button(handle, 1, GLFW_RELEASE, 0);
                    if (step == 8)
                    {
                        rotated = app.scene().camera();
                        EXPECT_NE(rotated.rotation(), focused.rotation());
                        button(handle, 2, GLFW_PRESS, 0);
                    }
                    if (step == 9) glfwSetCursorPos(handle, 730, 520);
                    if (step == 10) button(handle, 2, GLFW_RELEASE, 0);
                    if (step == 11)
                    {
                        EXPECT_NE(app.scene().camera().position(), rotated.position());
                        scroll(handle, 0, 1);
                    }
                    if (step == 13) button(handle, 1, GLFW_PRESS, 0);
                    if (step == 14) glfwSetCursorPos(handle, 710, 510);
                    if (step == 15)
                    {
                        beforeMinimize = app.scene().camera();
                        glfwIconifyWindow(handle);
                        app.window().pollEvents();
                        ASSERT_TRUE(app.window().isIconified());
                    }
                    if (step == 16) button(handle, 1, GLFW_RELEASE, 0);
                    if (step == 18)
                    {
                        glfwRestoreWindow(handle);
                        glfwFocusWindow(handle);
                        glfwSetWindowSize(handle, 600, 800);
                        app.window().pollEvents();
                        ASSERT_FALSE(app.window().isIconified());
                    }
                    if (step == 19) glfwSetCursorPos(handle, 500, 700);
                    if (step == 22)
                    {
                        EXPECT_EQ(app.scene().camera().position(), beforeMinimize.position());
                        EXPECT_EQ(app.scene().camera().rotation(), beforeMinimize.rotation());
                        EXPECT_GE(cancellations, 3);
                    }
                    if (step == 23) button(handle, 1, GLFW_PRESS, 0);
                    if (step == 24) glfwSetCursorPos(handle, 520, 700);
                    if (step == 25) button(handle, 1, GLFW_RELEASE, 0);
                    if (step == 26) EXPECT_NE(app.scene().camera().rotation(), beforeMinimize.rotation());
                    if (step == 29) ASSERT_NO_FATAL_FAILURE(checkFocus(app));
                    if (step >= 0)
                    {
                        EXPECT_EQ(state->opens, opens);
                        EXPECT_EQ(count("Uploaded mesh:"), uploads);
                    }
                    if (step == 30)
                    {
                        std::cout << (obj ? "Teapot" : "Avocado")
                                  << ": production input/focus/culling, minimized release, portrait refocus passed\n";
                        app.requestExit();
                    }
                },
                [&](Demo::DemoApp& app) { demo.ui(app); glfwWaitEventsTimeout(0.04); },
                [&](Demo::DemoApp&) { ++cancellations; demo.cancelInteraction(); },
                [&](Demo::DemoApp& app, auto extent, const auto& input) {
                    demo.input(app, extent, input);
                }});
        }
        cleanValidation();
    }

    TEST_F(DemoAppTest, ModelDemoProductionControlsDriveSharedEntitiesAndSceneReplacement)
    {
        copyShowcaseModels();
        auto config = options(true);
        config.window.width = 1280;
        config.window.height = 720;
        auto state = std::make_shared<Lifetime>();
        Demo::DemoApp session(config, std::make_unique<TrackedFileIO>(state));
        auto demo = std::make_shared<Demo::ModelScene>();
        const std::weak_ptr<Demo::ModelScene> demoLifetime = demo;
        Scene::EntityId first, second, created;
        Scene::Transform left, right, moved;
        constexpr std::array<int, 3> CLEAR{51, 61, 71};
        size_t preparedMeshUploads = 0, preparedTextureUploads = 0, preparedMaterialUploads = 0;
        size_t ordinaryOpens = 0;
        int updates = 0;
        const auto visiblePixels = [&](const WindowCapture::Image& image, Demo::DemoApp& app,
                                       const Scene::Transform& transform) {
            const auto camera = app.scene().camera().matrices({1280, 720});
            const auto clip = camera.projection * camera.view * glm::vec4(transform.position(), 1);
            if (clip.w <= 0)
                return size_t{0};
            const int centerX = static_cast<int>((clip.x / clip.w + 1.0f) * image.width * 0.5f);
            const int centerY = static_cast<int>((1.0f - clip.y / clip.w) * image.height * 0.5f);
            const int width = static_cast<int>(image.width), height = static_cast<int>(image.height);
            size_t count = 0;
            for (int y = std::max(0, centerY - 36); y < std::min(height, centerY + 36); ++y)
                for (int x = std::max(0, centerX - 36); x < std::min(width, centerX + 36); ++x)
                {
                    const auto pixel = (static_cast<size_t>(y) * image.width + x) * 4;
                    if (std::abs(static_cast<int>(image.rgba[pixel]) - CLEAR[0]) > 12 ||
                        std::abs(static_cast<int>(image.rgba[pixel + 1]) - CLEAR[1]) > 12 ||
                        std::abs(static_cast<int>(image.rgba[pixel + 2]) - CLEAR[2]) > 12)
                        ++count;
                }
            return count;
        };
        const auto capture = [&](Demo::DemoApp& app, const char* name) {
            glfwWaitEventsTimeout(0.06);
            auto image = WindowCapture::capture(app.window());
            WindowCapture::save(image, fs::path("review") / name);
            return image;
        };
        const auto checkCulling = [&](Demo::DemoApp& app, size_t visible) {
            int width, height;
            app.window().getFramebufferSize(width, height);
            const Render::FrameExtent extent{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
            const auto frame = Scene::extractSceneFrame(app.scene(), extent);
            const auto all = Scene::extractSceneFrame(app.scene(), extent, false);
            ASSERT_TRUE(frame);
            ASSERT_TRUE(all);
            const auto* entity = app.scene().find(first);
            ASSERT_NE(entity, nullptr);
            const auto& model = *entity->model;
            EXPECT_EQ(frame->draws.size(), model.draws().size() * visible);
            EXPECT_EQ(all->draws.size(), model.draws().size() * 2);
            if (visible == 1)
                for (size_t i = 0; i < model.draws().size(); ++i)
                    EXPECT_EQ(frame->draws[i].model, right.matrix() * model.draws()[i].model);
            EXPECT_TRUE(entity->visible);
            EXPECT_EQ(app.scene().entities().size(), 2u);
            EXPECT_EQ(entity->model, app.scene().find(second)->model);
            EXPECT_EQ(entity->transform.scale(), left.scale());
            std::cout << "DEFAULT_CULLING visible=" << visible << " draws=" << frame->draws.size()
                      << " unculled=" << all->draws.size() << '\n';
        };
        const auto moveOffscreen = [&](Demo::DemoApp& app, Scene::EntityId id, Scene::Transform transform) {
            transform.setPosition(transform.position() + glm::vec3(1000, 0, 0));
            EXPECT_TRUE(app.scene().setTransform(id, transform));
        };
        const auto checkEmptyImage = [&](Demo::DemoApp& app, const char* name) {
            const auto image = capture(app, name);
            EXPECT_EQ(visiblePixels(image, app, left), 0u);
            EXPECT_EQ(visiblePixels(image, app, right), 0u);
            const size_t pixel = (32u * image.width + 1200) * 4;
            EXPECT_EQ(image.rgba[pixel], 255);
            EXPECT_EQ(image.rgba[pixel + 1], 0);
            EXPECT_EQ(image.rgba[pixel + 2], 255);
        };
        Demo::Callbacks callbacks{
            [&, demo](Demo::DemoApp& app) {
                demo->initialize(app);
                ASSERT_EQ(app.scene().entities().size(), 2u);
                first = demo->selected();
                second = app.scene().entities()[1].id;
                left = app.scene().find(first)->transform;
                right = app.scene().find(second)->transform;
                EXPECT_EQ(app.scene().find(first)->model, app.scene().find(second)->model);
                track(app, app.scene().find(first)->model, state);
                unloaded(ResourceType::MODEL, DEMO_GLTF);
                unloaded(ResourceType::TEXTURE, DEMO_PNG);
                EXPECT_FALSE(ResourceRegistry::instance().lookup(ResourceType::MODEL, DEMO_OBJ).valid());
                ordinaryOpens = state->opens;
            },
            [&, demo](Demo::DemoApp& app, const Demo::FrameTime& time) {
                const int frame = updates++;
                demo->update(app, time);
                if (frame == 0)
                    EXPECT_EQ(state->opens, ordinaryOpens);
                if (frame == 1)
                {
                    EXPECT_GT(state->opens, ordinaryOpens);
                    ordinaryOpens = state->opens;
                    unloaded(ResourceType::MODEL, DEMO_OBJ);
                    unloaded(ResourceType::TEXTURE, DEMO_PNG);
                    preparedMeshUploads = count("Uploaded mesh:");
                    preparedTextureUploads = count("Uploaded RGBA8 texture:");
                    preparedMaterialUploads = count("Created material:");
                    EXPECT_GT(preparedMeshUploads, 0u);
                    EXPECT_GT(preparedTextureUploads, 0u);
                    EXPECT_GT(preparedMaterialUploads, 0u);
                }
                EXPECT_EQ(state->opens, ordinaryOpens);
                routes();
                if (frame == 3)
                {
                    const auto image = capture(app, "scene-demo-shared.ppm");
                    EXPECT_GT(visiblePixels(image, app, left), 0u);
                    EXPECT_GT(visiblePixels(image, app, right), 0u);
                    EXPECT_EQ(count("Uploaded mesh:"), preparedMeshUploads);
                    EXPECT_EQ(count("Uploaded RGBA8 texture:"), preparedTextureUploads);
                    EXPECT_EQ(count("Created material:"), preparedMaterialUploads);
                }
                if (frame == 6)
                {
                    const auto image = capture(app, "scene-demo-hidden.ppm");
                    EXPECT_EQ(visiblePixels(image, app, left), 0u);
                    EXPECT_GT(visiblePixels(image, app, right), 0u);
                    ASSERT_TRUE(app.scene().setVisible(first, true));
                    moved = Scene::Transform({-0.5f, -0.3f, 0}, glm::quat(glm::vec3(0, 0, 0.3f)), {12, 16.8f, 24});
                    ASSERT_TRUE(app.scene().setTransform(first, moved));
                    EXPECT_EQ(app.scene().find(second)->transform.position(), right.position());
                }
                if (frame == 9)
                {
                    const auto image = capture(app, "scene-demo-transformed.ppm");
                    EXPECT_EQ(visiblePixels(image, app, left), 0u);
                    EXPECT_GT(visiblePixels(image, app, moved), 0u);
                    EXPECT_GT(visiblePixels(image, app, right), 0u);
                    demo->deleteSelected(app);
                    EXPECT_EQ(app.scene().find(first), nullptr);
                    EXPECT_FALSE(app.scene().setVisible(first, true));
                }
                if (frame == 12)
                {
                    const auto image = capture(app, "scene-demo-deleted.ppm");
                    EXPECT_EQ(visiblePixels(image, app, moved), 0u);
                    EXPECT_GT(visiblePixels(image, app, right), 0u);
                    demo->clear(app);
                    EXPECT_TRUE(app.scene().entities().empty());
                    EXPECT_EQ(app.scene().find(second), nullptr);
                }
                if (frame == 15)
                {
                    const auto image = capture(app, "scene-demo-empty.ppm");
                    EXPECT_EQ(visiblePixels(image, app, right), 0u);
                    created = demo->createEntity(app);
                    EXPECT_NE(created, first);
                    EXPECT_NE(created, second);
                    EXPECT_EQ(app.scene().entities().size(), 1u);
                }
                if (frame == 18)
                {
                    const auto image = capture(app, "scene-demo-created.ppm");
                    EXPECT_GT(visiblePixels(image, app, app.scene().find(created)->transform), 0u);
                    demo->replaceCamera(app);
                    EXPECT_FLOAT_EQ(app.scene().camera().position().z, 8);
                }
                if (frame == 21)
                {
                    const auto image = capture(app, "scene-demo-camera.ppm");
                    EXPECT_GT(visiblePixels(image, app, app.scene().find(created)->transform), 0u);
                    demo->switchScene(app, true);
                    EXPECT_EQ(app.scene().find(created), nullptr);
                    EXPECT_FALSE(app.scene().setTransform(created, Scene::Transform{}));
                    EXPECT_FLOAT_EQ(app.scene().camera().position().z, 6);
                    ASSERT_EQ(app.scene().entities().size(), 2u);
                    EXPECT_EQ(app.scene().entities()[0].model, app.scene().entities()[1].model);
                    track(app, app.scene().entities()[0].model, state);
                    left = app.scene().entities()[0].transform;
                    right = app.scene().entities()[1].transform;
                    EXPECT_EQ(count("Uploaded mesh:"), preparedMeshUploads);
                    EXPECT_EQ(count("Uploaded RGBA8 texture:"), preparedTextureUploads);
                    EXPECT_EQ(count("Created material:"), preparedMaterialUploads);
                }
                if (frame == 24)
                {
                    const auto image = capture(app, "scene-demo-obj.ppm");
                    EXPECT_GT(visiblePixels(image, app, left), 0u);
                    EXPECT_GT(visiblePixels(image, app, right), 0u);
                    demo->switchScene(app, false);
                    left = app.scene().entities()[0].transform;
                    right = app.scene().entities()[1].transform;
                }
                if (frame == 27)
                {
                    const auto image = capture(app, "scene-demo-return-gltf.ppm");
                    EXPECT_GT(visiblePixels(image, app, left), 0u);
                    EXPECT_GT(visiblePixels(image, app, right), 0u);
                    first = app.scene().entities()[0].id;
                    second = app.scene().entities()[1].id;
                    moveOffscreen(app, first, left);
                    checkCulling(app, 1);
                }
                if (frame == 30 || frame == 39)
                {
                    const auto image = capture(app, frame == 30 ? "gltf-culled.ppm" : "obj-culled.ppm");
                    EXPECT_EQ(visiblePixels(image, app, left), 0u);
                    EXPECT_GT(visiblePixels(image, app, right), 0u);
                    moveOffscreen(app, second, right);
                    checkCulling(app, 0);
                }
                if (frame == 33 || frame == 42)
                {
                    checkEmptyImage(app, frame == 33 ? "gltf-all-culled-ui.ppm" : "obj-all-culled-ui.ppm");
                    ASSERT_TRUE(app.scene().setTransform(first, left));
                    ASSERT_TRUE(app.scene().setTransform(second, right));
                    checkCulling(app, 2);
                }
                if (frame == 36 || frame == 45)
                {
                    const auto image = capture(app, frame == 36 ? "gltf-returned.ppm" : "obj-returned.ppm");
                    EXPECT_GT(visiblePixels(image, app, left), 0u);
                    EXPECT_GT(visiblePixels(image, app, right), 0u);
                    if (frame == 45)
                        app.requestExit();
                    else
                    {
                        demo->switchScene(app, true);
                        first = app.scene().entities()[0].id;
                        second = app.scene().entities()[1].id;
                        left = app.scene().find(first)->transform;
                        right = app.scene().find(second)->transform;
                        moveOffscreen(app, first, left);
                        checkCulling(app, 1);
                    }
                }
            },
            [&, demo](Demo::DemoApp& app) {
                // UI-side entity edits take effect in this frame's production extraction.
                if (updates == 4)
                    ASSERT_TRUE(app.scene().setVisible(first, false));
                demo->ui(app);
                if (updates >= 28)
                    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(1180, 16), ImVec2(1220, 48),
                                                                  IM_COL32(255, 0, 255, 255));
                glfwWaitEventsTimeout(0.04);
            }};
        demo.reset();
        session.run(std::move(callbacks));
        EXPECT_EQ(updates, 46);
        EXPECT_TRUE(demoLifetime.expired());
        closed(state);
        unloaded(ResourceType::MODEL, DEMO_GLTF);
        unloaded(ResourceType::MODEL, DEMO_OBJ);
        unloaded(ResourceType::TEXTURE, DEMO_PNG);
        EXPECT_EQ(count("Uploaded mesh:"), preparedMeshUploads);
        EXPECT_EQ(count("Uploaded RGBA8 texture:"), preparedTextureUploads);
        EXPECT_EQ(count("Created material:"), preparedMaterialUploads);
        EXPECT_EQ(count("Created graphics pipeline:"), 1u);
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
                    session.requestExit();  // Destruction without run is also a normal close path.
            }
            EXPECT_EQ(updates, mode < 2 ? 3 : mode == 2 ? 1 : 0);
            EXPECT_EQ(uis, mode < 2 ? 2 : mode == 2 ? 1 : 0);
            closed(state);
        }
        cleanValidation();
    }
}  // namespace
