#include "runtime/application/application.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <spdlog/sinks/ostream_sink.h>

#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/model/prepared_model.h"
#include "runtime/resource/loader/text_resource.h"
#include "runtime/resource/loader/texture_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"
#include "runtime/scene/scene_frame.h"
#include "support/window_capture.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource;
    namespace fs = std::filesystem;
    namespace NativeIO = Incubator::FileIO;
    constexpr const char* GLTF = "models/minimal_gltf/model.gltf";
    constexpr const char* OBJ = "models/minimal_obj/model.obj";
    constexpr const char* PNG = "textures/direction_rgba.png";

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

    class ApplicationTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            previous_ = fs::current_path();
            sandbox_ =
                previous_ / "review/application" / ::testing::UnitTest::GetInstance()->current_test_info()->name();
            fs::remove_all(sandbox_);
            fs::create_directories(sandbox_ / "assets");
            for (const char* directory : {"models", "textures", "shaders"})
                fs::copy(previous_ / "assets" / directory, sandbox_ / "assets" / directory,
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
        Application::SessionOptions options(bool visible = false)
        {
            Application::SessionOptions result;
            result.window.title = "Incubator application session";
            result.window.width = 640;
            result.window.height = 480;
            result.window.visible = visible;
            result.validation = Render::RendererValidationMode::Required;
            result.clearColor = {0.2f, 0.24f, 0.28f, 1};
            return result;
        }
        void track(Application::Session& session, const std::shared_ptr<const Model::PreparedModel>& model,
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

    TEST_F(ApplicationTest, AnyExistingRouteRejectsTakeoverWithoutChangingOriginalLoader)
    {
        for (const auto type : {ResourceType::TEXT, static_cast<ResourceType>(0x12345678)})
        {
            auto& manager = ResourceManager::instance();
            auto destroyed = std::make_shared<bool>(false);
            manager.registerLoader(type, std::make_unique<OriginalLoader>(type, destroyed));
            auto state = std::make_shared<Lifetime>();
            try
            {
                Application::Session session(options(), std::make_unique<TrackedFileIO>(state));
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

    TEST_F(ApplicationTest, ShaderInitializationFailureReleasesHoldsAndOwnedRoutesBeforeFileIO)
    {
        auto state = std::make_shared<Lifetime>();
        auto config = options();
        config.vertexShader = "shaders/missing.vert.spv";
        try
        {
            Application::Session session(config, std::make_unique<TrackedFileIO>(state));
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

    TEST_F(ApplicationTest, LiveRoutesAllowLaterModelPreparationAndSceneSwitchWithOtherCpuOwner)
    {
        auto state = std::make_shared<Lifetime>();
        Application::Session session(options(true), std::make_unique<TrackedFileIO>(state));
        Scene::EntityId first;
        ResourceId otherPng;
        const TextureResource* otherPixels = nullptr;
        size_t ordinaryOpens = 0;
        int updates = 0, uis = 0;
        std::vector<char> order;
        session.run(
            {[&](Application::Session& app) {
                 routes();
                 unloaded(ResourceType::SHADER, "shaders/cube.vert.spv");
                 unloaded(ResourceType::SHADER, "shaders/cube.frag.spv");
                 const auto model = Model::prepareModel(app.renderer(), GLTF);
                 track(app, model, state);
                 app.scene().setCamera(Scene::Camera({0, 0, 1}, {1, 0, 0, 0}, {glm::radians(90.0f), 0.1f, 10}));
                 first = app.scene().createEntity(model,
                                                  Scene::Transform({-0.65f, 0.45f, 0}, {1, 0, 0, 0}, glm::vec3(0.25f)));
                 app.scene().createEntity(model, Scene::Transform({0.65f, 0.45f, 0}, {1, 0, 0, 0}, glm::vec3(0.25f)));
                 unloaded(ResourceType::MODEL, GLTF);
                 unloaded(ResourceType::TEXTURE, PNG);
                 otherPng = ResourceRegistry::instance().lookup(ResourceType::TEXTURE, PNG);
                 ResourceManager::instance().acquire(otherPng);
                 otherPixels = ResourceManager::instance().get<TextureResource>(otherPng);
                 ASSERT_NE(otherPixels, nullptr);
                 ordinaryOpens = state->opens;
                 order.push_back('i');
             },
             [&](Application::Session& app, const Application::FrameTime&) {
                 const int frame = updates++;
                 order.push_back('u');
                 routes();
                 EXPECT_EQ(state->opens, ordinaryOpens);
                 if (frame == 2)
                 {
                     glfwWaitEventsTimeout(0.06);
                     auto image = WindowCapture::capture(app.window());
                     color(image, -0.510897f, 0.567851f, {51, 61, 71});
                     color(image, 0.789103f, 0.567851f, {188, 255, 188});
                     WindowCapture::save(image, "review/application-ui-edit.ppm");
                     const auto model = Model::prepareModel(app.renderer(), OBJ);
                     EXPECT_EQ(ResourceRegistry::instance().refCount(otherPng), 1u);
                     EXPECT_EQ(ResourceRegistry::instance().getObject(otherPng), otherPixels);
                     app.scene().clear();
                     routes();
                     auto next = std::make_unique<Scene::Scene>();
                     next->setCamera(Scene::Camera({0, 0, 2}, {1, 0, 0, 0}, {glm::radians(90.0f), 0.1f, 10}));
                     next->createEntity(model, Scene::Transform({-0.6f, -0.2f, 0}, {1, 0, 0, 0}, glm::vec3(0.6f)));
                     next->createEntity(model, Scene::Transform({0.6f, -0.2f, 0}, {1, 0, 0, 0}, glm::vec3(0.6f)));
                     app.setScene(std::move(next));
                     EXPECT_EQ(app.scene().find(first), nullptr);
                     EXPECT_FALSE(app.scene().setVisible(first, true));
                     EXPECT_FLOAT_EQ(app.scene().camera().position().z, 2);
                     track(app, model, state);
                     ordinaryOpens = state->opens;
                     unloaded(ResourceType::MODEL, OBJ);
                 }
                 if (frame == 5)
                 {
                     glfwWaitEventsTimeout(0.06);
                     const auto image = WindowCapture::capture(app.window());
                     color(image, -0.12f, -0.15f, {188, 255, 188});
                     color(image, 0.48f, -0.15f, {188, 255, 188});
                     color(image, 0.789103f, 0.567851f, {51, 61, 71});
                     WindowCapture::save(image, "review/application-switched-scene.ppm");
                     app.requestExit();
                 }
             },
             [&](Application::Session& app) {
                 order.push_back('g');
                 if (uis++ == 1)
                     ASSERT_TRUE(app.scene().setVisible(first, false));
                 glfwWaitEventsTimeout(0.04);
             }});
        EXPECT_FALSE(session.isOpen());
        EXPECT_EQ(updates, 6);
        EXPECT_EQ(uis, 5);
        EXPECT_EQ(order, (std::vector<char>{'i', 'u', 'g', 'u', 'g', 'u', 'g', 'u', 'g', 'u', 'g', 'u'}));
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

    TEST_F(ApplicationTest, MinimizedLoopsAdvanceTimeAndUpdateWhileSkippingUiThenRestoreNewExtent)
    {
        auto state = std::make_shared<Lifetime>();
        Application::Session session(options(true), std::make_unique<TrackedFileIO>(state));
        std::vector<Application::FrameTime> times;
        std::vector<int> uiUpdates;
        int updates = 0;
        size_t opens = 0;
        session.run({[&](Application::Session& app) {
                         const auto model = Model::prepareModel(app.renderer(), GLTF);
                         track(app, model, state);
                         app.scene().setCamera(Scene::Camera({0, 0, 1}, {1, 0, 0, 0}, {glm::radians(90.0f), 0.1f, 10}));
                         app.scene().createEntity(model, Scene::Transform({0, 0, 0}, {1, 0, 0, 0}, glm::vec3(0.5f)));
                         opens = state->opens;
                     },
                     [&](Application::Session& app, const Application::FrameTime& time) {
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
                     [&](Application::Session& app) {
                         uiUpdates.push_back(updates - 1);
                         EXPECT_FALSE(app.window().isIconified());
                         glfwWaitEventsTimeout(0.04);
                     }});
        EXPECT_EQ(updates, 11);
        EXPECT_EQ(uiUpdates, (std::vector<int>{0, 5, 6, 7, 8, 9}));
        closed(state);
        EXPECT_EQ(count("Uploaded mesh:"), 2u);
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

    TEST_F(ApplicationTest, InitializeUpdateAndUiExceptionsReleaseCallbacksAndResourcesBeforeFixtureCleanup)
    {
        for (int phase = 0; phase < 3; ++phase)
        {
            SCOPED_TRACE(phase);
            auto state = std::make_shared<Lifetime>();
            Application::Session session(options(), std::make_unique<TrackedFileIO>(state));
            auto hold = std::make_shared<CallbackHold>(state);
            int updates = 0, uis = 0;
            Application::Callbacks callbacks{
                [&, hold](Application::Session& app) {
                    auto model = Model::prepareModel(app.renderer(), GLTF);
                    track(app, model, state);
                    app.scene().createEntity(model);
                    hold->cpu = ResourceRegistry::instance().lookup(ResourceType::TEXTURE, PNG);
                    ResourceManager::instance().acquire(hold->cpu);
                    EXPECT_NE(ResourceManager::instance().get<TextureResource>(hold->cpu), nullptr);
                    if (phase == 0)
                        throw std::runtime_error("initialize failure");
                },
                [&, hold](Application::Session&, const Application::FrameTime&) {
                    ++updates;
                    if (phase == 1)
                        throw std::runtime_error("update failure");
                },
                [&, hold](Application::Session&) {
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

    TEST_F(ApplicationTest, MissingModelPngStopsSessionAndReleasesDependencyHolds)
    {
        fs::remove("assets/textures/direction_rgba.png");
        auto state = std::make_shared<Lifetime>();
        Application::Session session(options(), std::make_unique<TrackedFileIO>(state));
        int updates = 0;
        EXPECT_THROW(session.run({[&](Application::Session& app) {
                                      Model::prepareModel(app.renderer(), GLTF);
                                      app.requestExit();
                                  },
                                  [&](Application::Session&, const Application::FrameTime&) { ++updates; },
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

    TEST_F(ApplicationTest, ExitWindowCloseAndDestructionStopFurtherCallbacks)
    {
        for (int mode = 0; mode < 4; ++mode)
        {
            SCOPED_TRACE(mode);
            auto state = std::make_shared<Lifetime>();
            int updates = 0, uis = 0;
            {
                Application::Session session(options(), std::make_unique<TrackedFileIO>(state));
                auto model = Model::prepareModel(session.renderer(), GLTF);
                track(session, model, state);
                session.scene().createEntity(model);
                model.reset();
                if (mode < 3)
                    session.run({{},
                                 [&](Application::Session& app, const Application::FrameTime&) {
                                     if (++updates == 3 && mode < 2)
                                     {
                                         if (mode == 0)
                                             app.requestExit();
                                         else
                                             app.window().setShouldClose(true);
                                     }
                                 },
                                 [&](Application::Session& app) {
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
