#include "runtime/application/application.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "runtime/core/utils/utf8.h"
#include "examples/scene_demo/scene_demo.h"
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
                         fs::copy_options::recursive);
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

    TEST_F(ApplicationTest, LiveRoutesAllowLaterModelPreparationWithOtherCpuOwner)
    {
        auto state = std::make_shared<Lifetime>();
        Application::Session session(options(true), std::make_unique<TrackedFileIO>(state));
        ResourceId otherPng;
        const TextureResource* otherPixels = nullptr;
        size_t ordinaryOpens = 0;
        session.run(
            {[&](Application::Session& app) {
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
             [&](Application::Session& app, const Application::FrameTime&) {
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

    TEST_F(ApplicationTest, SceneDemoProductionControlsDriveSharedEntitiesAndSceneReplacement)
    {
        copyShowcaseModels();
        auto config = options(true);
        config.window.width = 1280;
        config.window.height = 720;
        auto state = std::make_shared<Lifetime>();
        Application::Session session(config, std::make_unique<TrackedFileIO>(state));
        auto demo = std::make_shared<SceneDemo::Demo>();
        const std::weak_ptr<SceneDemo::Demo> demoLifetime = demo;
        Scene::EntityId first, second, created;
        Scene::Transform left, right, moved;
        constexpr std::array<int, 3> CLEAR{51, 61, 71};
        size_t preparedMeshUploads = 0, preparedTextureUploads = 0, preparedMaterialUploads = 0;
        size_t ordinaryOpens = 0;
        int updates = 0;
        const auto visiblePixels = [&](const WindowCapture::Image& image, Application::Session& app,
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
        const auto capture = [&](Application::Session& app, const char* name) {
            glfwWaitEventsTimeout(0.06);
            auto image = WindowCapture::capture(app.window());
            WindowCapture::save(image, fs::path("review") / name);
            return image;
        };
        Application::Callbacks callbacks{
            [&, demo](Application::Session& app) {
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
            [&, demo](Application::Session& app, const Application::FrameTime& time) {
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
                    app.requestExit();
                }
            },
            [&, demo](Application::Session& app) {
                // UI-side entity edits take effect in this frame's production extraction.
                if (updates == 4)
                    ASSERT_TRUE(app.scene().setVisible(first, false));
                demo->ui(app);
                glfwWaitEventsTimeout(0.04);
            }};
        demo.reset();
        session.run(std::move(callbacks));
        EXPECT_EQ(updates, 28);
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
