#include <GLFW/glfw3.h>
#include <gtest/gtest.h>
#include <imgui.h>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <iostream>
#include <iterator>
#include <sstream>

#include "examples/demo/scene_assets.h"
#include "examples/demo/scene_controls.h"
#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/model/model_instance.h"
#include "runtime/model/prepared_model.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/platform/window/window.h"
#include "runtime/render/detail/renderer_review.h"
#include "runtime/render/integration/imgui_renderer.h"
#include "runtime/render/renderer.h"
#include "runtime/resource/loader/model_loader.h"
#include "runtime/resource/loader/shader_loader.h"
#include "runtime/resource/loader/shader_resource.h"
#include "runtime/resource/loader/texture_loader.h"
#include "runtime/resource/loader/texture_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"
#include "runtime/scene/scene.h"
#include "runtime/scene/scene_frame.h"
#include "support/texture_gpu_review.h"
#include "support/window_capture.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource;
    using namespace Incubator::Render;
    namespace fs = std::filesystem;
    constexpr const char* GLTF = "models/minimal_gltf/model.gltf";
    constexpr const char* OBJ = "models/minimal_obj/model.obj";
    constexpr const char* PNG = "textures/direction_rgba.png";

    size_t occurrences(std::string_view text, std::string_view value)
    {
        size_t count = 0;
        for (auto at = text.find(value); at != std::string_view::npos; at = text.find(value, at + value.size()))
            ++count;
        return count;
    }

    class SharedModelLoader final : public LoaderBase
    {
    public:
        explicit SharedModelLoader(bool empty = false) : empty_(empty) {}
        std::unique_ptr<ResourceObject> load(const ResourceId& id, std::string_view) override
        {
            MeshData mesh{{{{-0.8f, -0.6f, 0}, {0, 0}}, {{0.8f, -0.6f, 0}, {1, 0}}, {{0, 0.8f, 0}, {0.5f, 1}}},
                          {0, 1, 2}};
            std::vector<ModelMaterial> materials = {{PNG},
                                                    {"Textures/parent/../direction_rgba.png", {0.5f, 1, 0.5f}},
                                                    {std::nullopt, {0.5f, 0.5f, 0.5f}},
                                                    {std::nullopt},
                                                    {"textures/unused.png"}};
            std::vector<ModelPart> parts = {{0, 0}, {0, 1}, {0, 2}, {0, 3}, {1, 4}};
            std::vector<ModelNode> nodes = {
                {"root", glm::mat4(1), {1}, empty_ ? std::vector<uint32_t>{} : std::vector<uint32_t>{0, 1, 2, 3}},
                {"shared", glm::mat4(1), {}, empty_ ? std::vector<uint32_t>{} : std::vector<uint32_t>{0}}};
            return std::make_unique<ModelResource>(id, std::vector<MeshData>{mesh, mesh}, std::move(materials),
                                                   std::move(parts), std::move(nodes), 0);
        }

    private:
        bool empty_;
    };

    class ModelPreparationTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            previous_ = fs::current_path();
            sandbox_ = previous_ / "review/model-preparation" /
                       ::testing::UnitTest::GetInstance()->current_test_info()->name();
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
            auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(messages_);
            sink->set_pattern("%l|%v");
            for (const char* name : {"render", "render.vulkan"})
            {
                Log::Registry::get(name).addSink(sink);
                Log::Registry::get(name).setLevel(Log::Level::DEBUG);
            }
            Log::Registry::get("resource").addSink(std::make_shared<spdlog::sinks::ostream_sink_mt>(resourceMessages_));
            fileIO_ = FileIO::createPlatformFileIO();
            auto& manager = ResourceManager::instance();
            manager.registerLoader(ResourceType::MODEL, std::make_unique<ModelLoader>(*fileIO_));
            manager.registerLoader(ResourceType::TEXTURE, std::make_unique<TextureLoader>(*fileIO_));
            manager.registerLoader(ResourceType::SHADER, std::make_unique<ShaderLoader>(*fileIO_));
        }
        void TearDown() override
        {
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            fileIO_.reset();
            Log::Registry::reset();
            fs::current_path(previous_);
        }
        std::unique_ptr<Window::Window> window(bool visible = false)
        {
            Window::WindowDesc desc;
            desc.title = "Incubator model preparation";
            desc.width = 640;
            desc.height = 480;
            desc.visible = visible;
            return Window::createPlatformWindow(desc);
        }
        void shaders(Renderer& renderer)
        {
            auto& manager = ResourceManager::instance();
            const auto vertex = manager.registerResource(ResourceType::SHADER, "shaders/cube.vert.spv");
            const auto fragment = manager.registerResource(ResourceType::SHADER, "shaders/cube.frag.spv");
            const auto* v = manager.get<ShaderResource>(vertex);
            const auto* f = manager.get<ShaderResource>(fragment);
            ASSERT_NE(v, nullptr);
            ASSERT_NE(f, nullptr);
            renderer.prepareSceneResources(*v, *f);
            manager.release(vertex);
            manager.release(fragment);
            released(ResourceType::SHADER, "shaders/cube.vert.spv");
            released(ResourceType::SHADER, "shaders/cube.frag.spv");
        }
        void copyShowcaseModels()
        {
            for (const char* directory : {"utah_teapot", "avocado"})
                fs::copy(previous_ / "assets/models" / directory, sandbox_ / "assets/models" / directory,
                         fs::copy_options::recursive);
        }
        void released(ResourceType type, std::string_view path)
        {
            const auto id = ResourceRegistry::instance().lookup(type, path);
            ASSERT_TRUE(id.valid());
            EXPECT_EQ(ResourceRegistry::instance().refCount(id), 0u);
            EXPECT_EQ(ResourceRegistry::instance().getObject(id), nullptr);
        }
        void draw(Window::Window& window, Renderer& renderer, const Model::PreparedModel& model, size_t part = 0)
        {
            window.pollEvents();
            const auto extent = renderer.beginUiFrame();
            ASSERT_TRUE(extent);
            renderer.imgui().endFrame();
            const auto& item = model.parts()[part];
            const std::array draws{
                DrawItem{model.meshes()[item.geometry], model.materials()[item.material], glm::mat4(1)}};
            auto projection = glm::orthoRH_ZO(-4.0f / 3, 4.0f / 3, -1.0f, 1.0f, 0.1f, 10.0f);
            projection[1][1] *= -1;
            const auto view = glm::lookAtRH(glm::vec3(0, 0, 3), glm::vec3(0), glm::vec3(0, 1, 0));
            renderer.drawFrame({*extent, {view, projection}, draws}, {0.2f, 0.24f, 0.28f, 1});
        }
        void cleanValidation()
        {
            const auto text = messages_.str();
            std::cout << text;
            EXPECT_NE(text.find("validation: enabled"), std::string::npos);
            EXPECT_EQ(text.find("warning|"), std::string::npos) << text;
            EXPECT_EQ(text.find("error|"), std::string::npos) << text;
        }
        fs::path previous_, sandbox_;
        std::unique_ptr<FileIO::FileIO> fileIO_;
        std::ostringstream messages_, resourceMessages_;
    };

    TEST_F(ModelPreparationTest, BothImportedFormatsReleaseCpuAssetsAndDrawSharedResults)
    {
        auto native = window();
        std::vector<std::shared_ptr<const Model::PreparedModel>> models;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            for (const char* path : {GLTF, OBJ})
            {
                auto model = Model::prepareModel(renderer, path);
                EXPECT_EQ(model->meshes().size(), 2u);
                EXPECT_EQ(model->materials().size(), 2u);
                EXPECT_EQ(model->textures().size(), 2u);
                released(ResourceType::MODEL, path);
                released(ResourceType::TEXTURE, PNG);
                auto shared = model;
                model.reset();
                EXPECT_FALSE(shared->nodes().empty());
                for (int frame = 0; frame < 3; ++frame)
                    draw(*native, renderer, *shared, frame % 2);
                models.push_back(shared);
            }
            EXPECT_EQ(models[0]->nodes()[models[0]->rootNode()].name, "SourceRoot");
            EXPECT_FLOAT_EQ(models[0]->nodes()[models[0]->rootNode()].localTransform[3][0], 0.25f);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 4u);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded RGBA8 texture:"), 4u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 4u);
            EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline:"), 1u);
        }
        for (const auto& model : models)
        {
            for (const auto& mesh : model->meshes())
                EXPECT_FALSE(mesh.valid());
            for (const auto& material : model->materials())
                EXPECT_FALSE(material.valid());
            for (const auto& texture : model->textures())
                EXPECT_FALSE(texture.valid());
        }
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, FixedShowcaseModelsPrepareReleaseCpuAndDrawSharedInstances)
    {
        constexpr const char* TEAPOT = "models/utah_teapot/utah_teapot.obj";
        constexpr const char* AVOCADO = "models/avocado/glTF/Avocado.gltf";
        constexpr const char* AVOCADO_BASE_COLOR = "models/avocado/glTF/Avocado_baseColor.png";
        copyShowcaseModels();
        auto native = window();
        Scene::Scene scene;
        scene.setCamera(Scene::Camera({0, 0, 30}, {1, 0, 0, 0}, {glm::radians(45.0f), 0.1f, 100.0f}));
        std::vector<std::shared_ptr<const Model::PreparedModel>> models;
        size_t expectedMeshes = 0;
        size_t expectedTextures = 0;
        size_t expectedMaterials = 0;
        size_t expectedDraws = 0;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            size_t index = 0;
            for (const char* path : {TEAPOT, AVOCADO})
            {
                auto prepared = Model::prepareModel(renderer, path);
                ASSERT_NE(prepared, nullptr) << path;
                ASSERT_FALSE(prepared->meshes().empty()) << path;
                ASSERT_FALSE(prepared->materials().empty()) << path;
                ASSERT_FALSE(prepared->draws().empty()) << path;
                expectedMeshes += prepared->meshes().size();
                expectedTextures += prepared->textures().size();
                expectedMaterials += prepared->materials().size();
                expectedDraws += prepared->draws().size() * 2;

                const float scale = index == 0 ? 0.1f : 50.0f;
                const float y = index == 0 ? 3.0f : -3.0f;
                auto shared = prepared;
                scene.createEntity(shared, Scene::Transform({-3.5f, y, 0}, {1, 0, 0, 0}, glm::vec3(scale)));
                scene.createEntity(shared, Scene::Transform({3.5f, y, 0}, {1, 0, 0, 0}, glm::vec3(scale)));
                prepared.reset();
                models.push_back(std::move(shared));
                released(ResourceType::MODEL, path);
                ++index;
            }
            released(ResourceType::TEXTURE, AVOCADO_BASE_COLOR);

            for (int frameIndex = 0; frameIndex < 3; ++frameIndex)
            {
                native->pollEvents();
                const auto extent = renderer.beginUiFrame();
                ASSERT_TRUE(extent);
                auto frame = Scene::extractSceneFrame(scene, *extent);
                ASSERT_TRUE(frame);
                EXPECT_EQ(frame->draws.size(), expectedDraws);
                renderer.imgui().endFrame();
                renderer.drawFrame(frame->input(), {0.2f, 0.24f, 0.28f, 1});
                glfwWaitEventsTimeout(0.04);
            }
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), expectedMeshes);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded RGBA8 texture:"), expectedTextures);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), expectedMaterials);
            EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline:"), 1u);
            std::cout << "SHOWCASE_MODELS=2 SHARED_INSTANCES=4 CPU_ASSETS_RELEASED=1 FRAME_DRAWS=" << expectedDraws
                      << " REQUIRED_VALIDATION=1\n";
        }
        for (const auto& model : models)
        {
            for (const auto& mesh : model->meshes())
                EXPECT_FALSE(mesh.valid());
            for (const auto& material : model->materials())
                EXPECT_FALSE(material.valid());
            for (const auto& texture : model->textures())
                EXPECT_FALSE(texture.valid());
        }
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, SharesGeometryTexturesAndWhiteWhilePreservingOtherCpuOwners)
    {
        auto& manager = ResourceManager::instance();
        auto& registry = ResourceRegistry::instance();
        manager.registerLoader(ResourceType::MODEL, std::make_unique<SharedModelLoader>());
        const auto modelId = manager.registerResource(ResourceType::MODEL, "models/shared.gltf");
        const auto textureId = manager.registerResource(ResourceType::TEXTURE, PNG);
        const auto* cpu = manager.get<ModelResource>(modelId);
        const auto* pixels = manager.get<TextureResource>(textureId);
        ASSERT_NE(cpu, nullptr);
        ASSERT_NE(pixels, nullptr);
        auto native = window(true);
        std::shared_ptr<const Model::PreparedModel> model;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            model = Model::prepareModel(renderer, "models/shared.gltf");
            EXPECT_EQ(model->meshes().size(), 1u);
            EXPECT_EQ(model->materials().size(), 4u);
            EXPECT_EQ(model->textures().size(), 2u);
            EXPECT_EQ(model->parts().size(), 4u);
            EXPECT_EQ(model->nodes()[1].parts[0], model->nodes()[0].parts[0]);
            EXPECT_FALSE(registry.lookup(ResourceType::TEXTURE, "textures/unused.png").valid());
            EXPECT_EQ(registry.refCount(modelId), 1u);
            EXPECT_EQ(registry.refCount(textureId), 1u);
            EXPECT_EQ(registry.getObject(modelId), cpu);
            EXPECT_EQ(registry.getObject(textureId), pixels);
            EXPECT_TRUE(manager.hasLoader(ResourceType::MODEL));
            EXPECT_TRUE(manager.hasLoader(ResourceType::TEXTURE));
            manager.release(modelId);
            manager.release(textureId);
            released(ResourceType::MODEL, "models/shared.gltf");
            released(ResourceType::TEXTURE, PNG);
            manager.reset();
            for (int frame = 0; frame < 6; ++frame)
            {
                draw(*native, renderer, *model, 2);
                glfwWaitEventsTimeout(0.06);
            }
            const auto image = WindowCapture::capture(*native);
            const auto center = (static_cast<size_t>(image.height / 2) * image.width + image.width / 2) * 4;
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(image.rgba[center + channel], 188, 12);
            WindowCapture::save(image, "review/prepared-solid-gray.ppm");
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 1u);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded RGBA8 texture:"), 2u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 4u);
            EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline:"), 1u);
        }
        EXPECT_FALSE(model->meshes()[0].valid());
        EXPECT_FALSE(model->textures()[0].valid());
        EXPECT_FALSE(model->textures()[1].valid());
        EXPECT_FALSE(model->materials()[2].valid());
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, MissingAndInvalidPngFailBeforeGpuCreationAndPreserveOtherOwners)
    {
        auto& manager = ResourceManager::instance();
        auto& registry = ResourceRegistry::instance();
        const auto modelId = manager.registerResource(ResourceType::MODEL, GLTF);
        const auto otherId = manager.registerResource(ResourceType::TEXTURE, "textures/reference_rgba.png");
        const auto* source = manager.get<ModelResource>(modelId);
        const auto* other = manager.get<TextureResource>(otherId);
        ASSERT_NE(source, nullptr);
        ASSERT_NE(other, nullptr);
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            fs::remove(fs::path("assets") / PNG);
            for (bool corrupt : {false, true})
            {
                SCOPED_TRACE(corrupt);
                if (corrupt)
                {
                    std::ofstream output(fs::path("assets") / PNG);
                    output << "invalid PNG";
                }
                std::shared_ptr<const Model::PreparedModel> result;
                try
                {
                    result = Model::prepareModel(renderer, GLTF);
                    FAIL() << "Accepted failed dependency";
                }
                catch (const Error::Exception& error)
                {
                    EXPECT_EQ(error.code(), Error::Code::RENDER);
                    EXPECT_NE(error.message().find(GLTF), std::string::npos);
                    EXPECT_NE(error.message().find(PNG), std::string::npos);
                }
                EXPECT_EQ(result, nullptr);
                released(ResourceType::TEXTURE, PNG);
                EXPECT_EQ(registry.refCount(modelId), 1u);
                EXPECT_EQ(registry.refCount(otherId), 1u);
                EXPECT_EQ(registry.getObject(modelId), source);
                EXPECT_EQ(registry.getObject(otherId), other);
                EXPECT_TRUE(manager.hasLoader(ResourceType::MODEL));
                EXPECT_TRUE(manager.hasLoader(ResourceType::TEXTURE));
            }
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 0u);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded RGBA8 texture:"), 0u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 0u);
        }
        manager.release(modelId);
        manager.release(otherId);
        released(ResourceType::MODEL, GLTF);
        released(ResourceType::TEXTURE, "textures/reference_rgba.png");
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, MaterialFailureAfterUploadsReleasesCpuAndSameRendererRecovers)
    {
        auto native = window();
        std::shared_ptr<const Model::PreparedModel> model;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            try
            {
                model = Model::prepareModel(renderer, GLTF);
                FAIL() << "Accepted unprepared shaders";
            }
            catch (const Error::Exception& error)
            {
                EXPECT_EQ(error.code(), Error::Code::RENDER);
                EXPECT_NE(error.message().find("Prepare scene shaders"), std::string::npos);
            }
            EXPECT_EQ(model, nullptr);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded RGBA8 texture:"), 2u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 0u);
            released(ResourceType::MODEL, GLTF);
            released(ResourceType::TEXTURE, PNG);
            shaders(renderer);
            model = Model::prepareModel(renderer, GLTF);
            released(ResourceType::MODEL, GLTF);
            released(ResourceType::TEXTURE, PNG);
            for (int frame = 0; frame < 3; ++frame)
                draw(*native, renderer, *model);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded RGBA8 texture:"), 4u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 2u);
            EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline:"), 1u);
        }
        EXPECT_FALSE(model->meshes()[0].valid());
        EXPECT_FALSE(model->materials()[0].valid());
        EXPECT_FALSE(model->textures()[0].valid());
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, UnusedPartsAndDependenciesCreateNoGpuResources)
    {
        ResourceManager::instance().registerLoader(ResourceType::MODEL, std::make_unique<SharedModelLoader>(true));
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            const auto model = Model::prepareModel(renderer, "models/empty.gltf");
            EXPECT_TRUE(model->meshes().empty());
            EXPECT_TRUE(model->materials().empty());
            EXPECT_TRUE(model->textures().empty());
            EXPECT_TRUE(model->parts().empty());
            EXPECT_EQ(model->nodes().size(), 2u);
            released(ResourceType::MODEL, "models/empty.gltf");
            EXPECT_FALSE(ResourceRegistry::instance().lookup(ResourceType::TEXTURE, PNG).valid());
            EXPECT_FALSE(ResourceRegistry::instance().lookup(ResourceType::TEXTURE, "textures/unused.png").valid());
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded"), 0u);
        }
        cleanValidation();
    }
    TEST(ModelInstanceInputTest, EmptyInputIsEmptyAndNullModelIsRejected)
    {
        EXPECT_TRUE(Model::makeModelDraws({}).empty());
        const std::array<Model::ModelInstance, 1> missing{};
        try
        {
            Model::makeModelDraws(missing);
            FAIL() << "Accepted null prepared model";
        }
        catch (const Error::Exception& error)
        {
            EXPECT_EQ(error.code(), Error::Code::CONFIG);
        }
    }

    class TransformModelLoader final : public LoaderBase
    {
    public:
        std::unique_ptr<ResourceObject> load(const ResourceId& id, std::string_view) override
        {
            MeshData mesh{{{{0, 0, 0}, {0, 0}}, {{1, 0, 0}, {1, 0}}, {{0, 1, 0}, {0, 1}}}, {0, 1, 2}};
            const auto root = glm::translate(glm::mat4(1), glm::vec3(10, 0, 0));
            const auto parent = glm::rotate(glm::mat4(1), glm::radians(90.0f), glm::vec3(0, 0, 1));
            const auto leaf =
                glm::translate(glm::mat4(1), glm::vec3(2, 0, 0)) * glm::scale(glm::mat4(1), glm::vec3(2, 3, 1));
            return std::make_unique<ModelResource>(
                id, std::vector<MeshData>{mesh},
                std::vector<ModelMaterial>{{}, {std::nullopt, {0.5f, 1, 0.5f}}, {std::nullopt, {1, 0, 0}}},
                std::vector<ModelPart>{{0, 0}, {0, 1}, {0, 2}},
                std::vector<ModelNode>{{"parent", parent, {1}, {0}},
                                       {"leaf", leaf, {}, {1, 0}},
                                       {"root", root, {0, 3}, {2}},
                                       {"sibling", glm::translate(glm::mat4(1), glm::vec3(-1, 0, 0)), {}, {2}}},
                2);
        }
    };

    class ModelInstanceTest : public ModelPreparationTest
    {
    protected:
        void point(const glm::mat4& matrix, float x, float y)
        {
            const auto actual = matrix * glm::vec4(1, 1, 0, 1);
            EXPECT_NEAR(actual.x, x, 0.00001f);
            EXPECT_NEAR(actual.y, y, 0.00001f);
            EXPECT_FLOAT_EQ(actual.z, 0.0f);
            EXPECT_FLOAT_EQ(actual.w, 1.0f);
        }
        CameraMatrices camera(FrameExtent extent)
        {
            const float aspect = static_cast<float>(extent.width) / extent.height;
            auto projection = glm::orthoRH_ZO(-aspect, aspect, -1.0f, 1.0f, 0.1f, 10.0f);
            projection[1][1] *= -1;
            return {glm::lookAtRH(glm::vec3(0, 0, 3), glm::vec3(0), glm::vec3(0, 1, 0)), projection};
        }
        std::optional<FrameExtent> submit(Window::Window& window, Renderer& renderer,
                                          std::span<const Model::ModelInstance> instances, bool ui = false)
        {
            window.pollEvents();
            const auto extent = renderer.beginUiFrame();
            if (!extent)
                return std::nullopt;
            auto draws = Model::makeModelDraws(instances);
            if (ui)
                ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(16, 16), ImVec2(48, 48),
                                                              IM_COL32(255, 0, 255, 255));
            renderer.imgui().endFrame();
            auto* data = renderer.imgui().drawData();
            renderer.drawFrame({*extent, camera(*extent), draws}, {0.2f, 0.24f, 0.28f, 1}, [&](void* commands) {
                if (ui)
                    renderer.imgui().recordDrawData(data, commands);
            });
            // Mutate and destroy the submitted CPU storage before the GPU necessarily finishes.
            for (auto& draw : draws)
                draw = {};
            draws.clear();
            draws.shrink_to_fit();
            return extent;
        }
        void color(const WindowCapture::Image& image, float x, float y, const std::array<int, 3>& expected)
        {
            const int px = image.width / 2 + static_cast<int>(x * image.height / 2);
            const int py = image.height / 2 - static_cast<int>(y * image.height / 2);
            ASSERT_GE(px, 0);
            ASSERT_LT(px, image.width);
            ASSERT_GE(py, 0);
            ASSERT_LT(py, image.height);
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(image.rgba[(static_cast<size_t>(py) * image.width + px) * 4 + channel], expected[channel],
                            12);
        }
    };

    class SceneEntityTest : public ModelInstanceTest
    {};

    TEST_F(SceneEntityTest, SharedModelEntitiesKeepGpuAssetsUntilRendererShutdown)
    {
        auto native = window(true);
        Mesh mesh;
        Material material;
        Texture texture;
        std::weak_ptr<const Model::PreparedModel> weak;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            auto model = Model::prepareModel(renderer, GLTF);
            const auto* identity = model.get();
            const auto rootMatrix = model->nodes()[model->rootNode()].localTransform;
            const auto nodeCount = model->nodes().size();
            mesh = model->meshes()[0];
            material = model->materials()[0];
            texture = model->textures()[0];
            weak = model;
            released(ResourceType::MODEL, GLTF);
            released(ResourceType::TEXTURE, PNG);
            ResourceManager::instance().reset();
            fileIO_.reset();
            const auto resourceLog = resourceMessages_.str();

            Scene::Scene scene;
            const Scene::Transform left({-0.65f, 0.45f, 0}, {1, 0, 0, 0}, glm::vec3(0.25f));
            const Scene::Transform right({0.65f, 0.45f, 0}, {1, 0, 0, 0}, glm::vec3(0.25f));
            const auto first = scene.createEntity(model, left);
            const auto second = scene.createEntity(model);
            const auto logical = scene.createEntity();
            ASSERT_TRUE(scene.setTransform(second, right));
            ASSERT_TRUE(scene.setModel(logical, model));
            EXPECT_EQ(scene.find(first)->model.get(), identity);
            EXPECT_EQ(scene.find(second)->model.get(), identity);
            EXPECT_EQ(scene.find(logical)->model.get(), identity);
            EXPECT_EQ(scene.entities().size(), 3u);
            EXPECT_EQ(scene.find(first)->transform.matrix(), left.matrix());
            EXPECT_EQ(scene.find(first)->model->nodes().size(), nodeCount);
            EXPECT_EQ(scene.find(first)->model->nodes()[model->rootNode()].localTransform, rootMatrix);
            model.reset();
            EXPECT_EQ(weak.use_count(), 3);
            ASSERT_TRUE(scene.setModel(logical, {}));
            EXPECT_EQ(weak.use_count(), 2);

            ASSERT_TRUE(scene.destroyEntity(first));
            EXPECT_EQ(weak.use_count(), 1);
            EXPECT_EQ(scene.find(first), nullptr);
            EXPECT_EQ(scene.find(second)->model.get(), identity);
            EXPECT_EQ(scene.find(second)->transform.matrix(), right.matrix());

            scene.clear();
            EXPECT_TRUE(weak.expired());
            EXPECT_TRUE(mesh.valid());
            EXPECT_TRUE(material.valid());
            EXPECT_TRUE(texture.valid());
            EXPECT_EQ(resourceMessages_.str(), resourceLog);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 2u);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded RGBA8 texture:"), 2u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 2u);
            EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline:"), 1u);
        }
        EXPECT_TRUE(weak.expired());
        EXPECT_FALSE(mesh.valid());
        EXPECT_FALSE(material.valid());
        EXPECT_FALSE(texture.valid());
        cleanValidation();
    }

    class SceneFrameTest : public ModelInstanceTest
    {};

    TEST_F(SceneFrameTest, OrderedNoncommutingTransformsAndCopiedMovedSnapshotsOwnTheirDraws)
    {
        ResourceManager::instance().registerLoader(ResourceType::MODEL, std::make_unique<TransformModelLoader>());
        auto native = window();
        Scene::SceneFrame retained;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            auto model = Model::prepareModel(renderer, "models/transforms.gltf");
            ASSERT_EQ(model->rootNode(), 2u);
            ASSERT_EQ(model->draws().size(), 5u);
            const std::weak_ptr<const Model::PreparedModel> weak = model;
            ResourceManager::instance().reset();
            fileIO_.reset();
            const auto resourceLog = resourceMessages_.str();
            Scene::Scene scene;
            const Scene::Transform a({1, 2, 0}, {1, 0, 0, 1}, {2, 3, 1});
            const Scene::Transform b({-2, 1, 0}, {1, 0, 0, 0}, {0.5f, 2, 1});
            const auto firstId = scene.createEntity(model, a);
            scene.createEntity();
            scene.createEntity(model, Scene::Transform{}, false);
            const auto secondId = scene.createEntity(model, b);
            auto first = Scene::extractSceneFrame(scene, {800, 400});
            ASSERT_TRUE(first);
            ASSERT_EQ(first->draws.size(), 10u);
            const std::array<std::array<float, 2>, 5> cached = {{{11, 1}, {9, 1}, {7, 4}, {7, 4}, {10, 1}}};
            for (size_t index = 0; index < cached.size(); ++index)
            {
                point(first->draws[index].model, 1 - 3 * cached[index][1], 2 + 2 * cached[index][0]);
                point(first->draws[index + 5].model, -2 + 0.5f * cached[index][0], 1 + 2 * cached[index][1]);
            }
            auto copied = *first;
            ASSERT_NE(copied.draws.data(), first->draws.data());
            EXPECT_EQ(copied.input().draws.data(), copied.draws.data());
            auto moved = std::move(copied);
            EXPECT_EQ(moved.input().draws.data(), moved.draws.data());
            retained = moved;
            EXPECT_NE(retained.draws.data(), moved.draws.data());
            Scene::SceneFrame moveAssigned;
            moveAssigned = std::move(moved);
            EXPECT_EQ(moveAssigned.input().draws.data(), moveAssigned.draws.data());

            ASSERT_TRUE(scene.setTransform(firstId, Scene::Transform({3, 0, 0})));
            ASSERT_TRUE(scene.setVisible(secondId, false));
            scene.setCamera(Scene::Camera({0, 0, 5}));
            const auto next = Scene::extractSceneFrame(scene, {400, 400});
            ASSERT_TRUE(next);
            ASSERT_EQ(next->draws.size(), 5u);
            point(next->draws[0].model, 14, 1);
            EXPECT_NE(next->camera.view, first->camera.view);
            EXPECT_NE(next->camera.projection, first->camera.projection);
            EXPECT_EQ(first->extent, (FrameExtent{800, 400}));
            point(first->draws[0].model, -2, 24);
            point(moveAssigned.draws[5].model, 3.5f, 3);

            ASSERT_TRUE(scene.destroyEntity(firstId));
            ASSERT_TRUE(scene.setVisible(secondId, true));
            scene.createEntity(model, a);  // New entity follows the surviving second entity.
            const auto reordered = Scene::extractSceneFrame(scene, {800, 400});
            ASSERT_TRUE(reordered);
            ASSERT_EQ(reordered->draws.size(), 10u);
            point(reordered->draws[0].model, 3.5f, 3);
            point(reordered->draws[5].model, -2, 24);
            ASSERT_TRUE(scene.setModel(secondId, {}));
            EXPECT_EQ(Scene::extractSceneFrame(scene, {800, 400})->draws.size(), 5u);
            scene.clear();
            model.reset();
            EXPECT_TRUE(weak.expired());  // Frames retain values/GPU references, not PreparedModel.
            EXPECT_TRUE(Scene::extractSceneFrame(scene, {800, 400})->draws.empty());
            first.reset();
            point(retained.draws[0].model, -2, 24);
            EXPECT_EQ(retained.input().draws.data(), retained.draws.data());
            EXPECT_TRUE(retained.draws[0].mesh.valid());
            EXPECT_EQ(resourceMessages_.str(), resourceLog);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 1u);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded RGBA8 texture:"), 1u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 3u);
            EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline:"), 1u);
        }
        EXPECT_FALSE(retained.draws[0].mesh.valid());
        EXPECT_FALSE(retained.draws[0].material.valid());
        cleanValidation();
    }

    TEST_F(SceneFrameTest, ImportedFramesSubmitUiEditsCameraResizeEmptyAndExpiredReferences)
    {
        auto native = window(true);
        Scene::Scene scene;
        scene.setCamera(Scene::Camera({0, 0, 1}, {1, 0, 0, 0}, {glm::radians(90.0f), 0.1f, 10}));
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            const auto gltf = Model::prepareModel(renderer, GLTF);
            const auto obj = Model::prepareModel(renderer, OBJ);
            const auto placement = [](float x, float y, float size) {
                return Scene::Transform({x, y, 0}, {1, 0, 0, 0}, glm::vec3(size));
            };
            const auto first = scene.createEntity(gltf, placement(-0.65f, 0.45f, 0.25f));
            scene.createEntity(gltf, placement(0.65f, 0.45f, 0.25f));
            scene.createEntity(obj, placement(-0.65f, -0.55f, 0.35f));
            scene.createEntity(obj, placement(0.65f, -0.55f, 0.35f));
            scene.createEntity();
            released(ResourceType::MODEL, GLTF);
            released(ResourceType::MODEL, OBJ);
            released(ResourceType::TEXTURE, PNG);
            ResourceManager::instance().reset();
            fileIO_.reset();
            const auto resourceLog = resourceMessages_.str();
            const auto submitScene = [&](size_t count, bool editInUi = false, bool ui = false) {
                native->pollEvents();
                const auto extent = renderer.beginUiFrame();
                ASSERT_TRUE(extent);
                if (editInUi)
                    ASSERT_TRUE(scene.setVisible(first, false));
                if (ui)
                    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(16, 16), ImVec2(48, 48),
                                                                  IM_COL32(255, 0, 255, 255));
                auto frame = Scene::extractSceneFrame(scene, *extent);
                ASSERT_TRUE(frame);
                EXPECT_EQ(frame->extent, *extent);
                EXPECT_EQ(frame->draws.size(), count);
                EXPECT_NEAR(frame->camera.projection[0][0], float(extent->height) / extent->width, 0.00001f);
                renderer.imgui().endFrame();
                auto* data = renderer.imgui().drawData();
                renderer.drawFrame(frame->input(), {0.2f, 0.24f, 0.28f, 1}, [&](void* commands) {
                    if (ui)
                        renderer.imgui().recordDrawData(data, commands);
                });
                frame->draws.clear();
                frame->draws.shrink_to_fit();
                frame.reset();
                glfwWaitEventsTimeout(0.04);
            };
            for (int frame = 0; frame < 3; ++frame)
                ASSERT_NO_FATAL_FAILURE(submitScene(12));
            auto image = WindowCapture::capture(*native);
            color(image, -0.510897f, 0.567851f, {188, 255, 188});
            color(image, 0.789103f, 0.567851f, {188, 255, 188});
            color(image, -0.44f, -0.608333f, {188, 255, 188});
            WindowCapture::save(image, "review/scene-frame-models.ppm");
            for (int frame = 0; frame < 3; ++frame)
                ASSERT_NO_FATAL_FAILURE(submitScene(8, true));
            image = WindowCapture::capture(*native);
            color(image, -0.510897f, 0.567851f, {51, 61, 71});
            color(image, 0.789103f, 0.567851f, {188, 255, 188});
            ASSERT_TRUE(scene.setVisible(first, true));

            // An extent changed between extraction and submission must skip the stale input.
            const auto oldExtent = renderer.beginUiFrame();
            ASSERT_TRUE(oldExtent);
            auto stale = Scene::extractSceneFrame(scene, *oldExtent);
            ASSERT_TRUE(stale);
            renderer.imgui().endFrame();
            glfwSetWindowSize(static_cast<GLFWwindow*>(native->getNativeHandle()), 800, 400);
            native->pollEvents();
            bool recorded = false;
            renderer.drawFrame(stale->input(), {0, 0, 0, 1}, [&](void*) { recorded = true; });
            EXPECT_FALSE(recorded);
            for (int frame = 0; frame < 4; ++frame)
            {
                ASSERT_TRUE(scene.setTransform(first, placement(-0.65f + (frame % 2 ? 0 : 0.03f), 0.45f, 0.25f)));
                ASSERT_NO_FATAL_FAILURE(submitScene(12));
            }
            scene.setCamera(Scene::Camera({0, 0, 2}, {1, 0, 0, 0}, {glm::radians(90.0f), 0.1f, 10}));
            for (int frame = 0; frame < 3; ++frame)
                ASSERT_NO_FATAL_FAILURE(submitScene(12));
            image = WindowCapture::capture(*native);
            EXPECT_EQ(image.width, 800);
            EXPECT_EQ(image.height, 400);
            color(image, 0.789103f / 2, 0.567851f / 2, {188, 255, 188});
            WindowCapture::save(image, "review/scene-frame-camera-resize.ppm");

            auto retained = Scene::extractSceneFrame(scene, {800, 400});
            ASSERT_TRUE(retained);
            scene.clear();
            ASSERT_TRUE(renderer.beginUiFrame());
            renderer.imgui().endFrame();
            renderer.drawFrame(retained->input(), {0.2f, 0.24f, 0.28f, 1});
            retained.reset();
            glfwWaitEventsTimeout(0.04);
            color(WindowCapture::capture(*native), 0.789103f / 2, 0.567851f / 2, {188, 255, 188});
            for (int frame = 0; frame < 3; ++frame)
                ASSERT_NO_FATAL_FAILURE(submitScene(0, false, true));
            image = WindowCapture::capture(*native);
            color(image, 0.789103f / 2, 0.567851f / 2, {51, 61, 71});
            const size_t uiPixel = (32u * image.width + 32) * 4;
            EXPECT_EQ(image.rgba[uiPixel], 255);
            EXPECT_EQ(image.rgba[uiPixel + 1], 0);
            EXPECT_EQ(image.rgba[uiPixel + 2], 255);
            WindowCapture::save(image, "review/scene-frame-empty-ui.ppm");
            scene.createEntity(gltf);
            ASSERT_NO_FATAL_FAILURE(submitScene(4));
            EXPECT_EQ(resourceMessages_.str(), resourceLog);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 4u);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded RGBA8 texture:"), 4u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 4u);
            EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline:"), 1u);
        }
        auto expired = Scene::extractSceneFrame(scene, {800, 400});
        ASSERT_TRUE(expired);
        ASSERT_EQ(expired->draws.size(), 4u);
        EXPECT_FALSE(expired->draws[0].mesh.valid());
        {
            Renderer next(*native, RendererValidationMode::Required);
            try
            {
                next.drawFrame(expired->input(), {0, 0, 0, 1});
                FAIL() << "Accepted expired Scene GPU references";
            }
            catch (const Error::Exception& error)
            {
                EXPECT_EQ(error.code(), Error::Code::RENDER);
                EXPECT_NE(error.message().find("Mesh is invalid"), std::string::npos);
            }
        }
        cleanValidation();
    }

    TEST_F(ModelInstanceTest, NonzeroRootDepthFirstPartsAndIndependentInstanceMatrices)
    {
        ResourceManager::instance().registerLoader(ResourceType::MODEL, std::make_unique<TransformModelLoader>());
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            auto model = Model::prepareModel(renderer, "models/transforms.gltf");
            released(ResourceType::MODEL, "models/transforms.gltf");
            EXPECT_EQ(model->rootNode(), 2u);
            ASSERT_EQ(model->draws().size(), 5u);
            EXPECT_EQ(model->meshes().size(), 1u);
            EXPECT_EQ(model->materials().size(), 3u);
            EXPECT_EQ(model->textures().size(), 1u);
            const std::array<std::array<float, 2>, 5> expected = {{{11, 1}, {9, 1}, {7, 4}, {7, 4}, {10, 1}}};
            for (size_t i = 0; i < expected.size(); ++i)
                point(model->draws()[i].model, expected[i][0], expected[i][1]);
            std::array instances = {Model::ModelInstance{model, glm::translate(glm::mat4(1), glm::vec3(0, 5, 0))},
                                    Model::ModelInstance{model, glm::translate(glm::mat4(1), glm::vec3(-2, 1, 0)) *
                                                                    glm::scale(glm::mat4(1), glm::vec3(0.5f, 2, 1))}};
            model.reset();
            EXPECT_EQ(instances[0].model.get(), instances[1].model.get());
            const auto draws = Model::makeModelDraws(instances);
            ASSERT_EQ(draws.size(), 10u);
            for (size_t i = 0; i < expected.size(); ++i)
            {
                point(draws[i].model, expected[i][0], expected[i][1] + 5);
                point(draws[i + 5].model, expected[i][0] * 0.5f - 2, expected[i][1] * 2 + 1);
            }
            instances[0].transform = glm::mat4(1);
            const auto changed = Model::makeModelDraws(instances);
            EXPECT_NE(changed[2].model, draws[2].model);
            EXPECT_EQ(changed[7].model, draws[7].model);
            point(instances[0].model->draws()[2].model, 7, 4);
            EXPECT_TRUE(Model::makeModelDraws({}).empty());
            ResourceManager::instance().registerLoader(ResourceType::MODEL, std::make_unique<SharedModelLoader>(true));
            const std::array empty{Model::ModelInstance{Model::prepareModel(renderer, "models/empty.gltf")}};
            EXPECT_TRUE(Model::makeModelDraws(empty).empty());
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 1u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 3u);
        }
        cleanValidation();
    }

    TEST_F(ModelInstanceTest, ImportedInstancesPresentMaterialsDepthAndSurviveListAndSwapchainChanges)
    {
        // A temporary OBJ variant gives the second format a distinct solid color for depth checks.
        {
            const fs::path path("assets/models/minimal_obj/materials/model.mtl");
            std::ifstream input(path);
            std::string mtl{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            input.close();
            const auto at = mtl.find("Kd 0.5 1 0.5");
            ASSERT_NE(at, std::string::npos);
            mtl.replace(at, std::string_view("Kd 0.5 1 0.5").size(), "Kd 1 0 0");
            std::ofstream(path) << mtl;
        }
        auto native = window(true);
        std::vector<Model::ModelInstance> instances;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            auto gltf = Model::prepareModel(renderer, GLTF);
            auto obj = Model::prepareModel(renderer, OBJ);
            ASSERT_EQ(gltf->draws().size(), 4u);
            ASSERT_EQ(obj->draws().size(), 2u);
            released(ResourceType::MODEL, GLTF);
            released(ResourceType::MODEL, OBJ);
            released(ResourceType::TEXTURE, PNG);
            const auto placement = [](float x, float y, float size) {
                return glm::translate(glm::mat4(1), glm::vec3(x, y, 0)) * glm::scale(glm::mat4(1), glm::vec3(size));
            };
            instances = {{gltf, placement(-0.65f, 0.45f, 0.25f)},
                         {gltf, placement(0.65f, 0.45f, 0.25f)},
                         {obj, placement(-0.65f, -0.55f, 0.35f)},
                         {obj, placement(0.65f, -0.55f, 0.35f)}};
            const auto first = Model::makeModelDraws(instances);
            ASSERT_EQ(first.size(), 12u);
            const auto k = std::sqrt(0.5f);
            point(first[0].model, -0.65f + 0.25f * (0.25f + 0.15f * k), 0.45f + 0.25f * 1.55f * k);
            point(first[4].model, 0.65f + 0.25f * (0.25f + 0.15f * k), 0.45f + 0.25f * 1.55f * k);
            point(first[2].model, -0.65f + 0.25f * (0.25f + 1.5f * k), 0.45f + 0.125f * k);
            point(first[8].model, -0.3f, -0.2f);
            // No CPU asset route or FileIO survives into the instance/frame loop.
            ResourceManager::instance().reset();
            fileIO_.reset();
            auto* handle = static_cast<GLFWwindow*>(native->getNativeHandle());
            for (int frame = 0; frame < 8; ++frame)
            {
                if (frame == 4)
                    glfwSetWindowSize(handle, 800, 600);
                instances[0].transform[3][0] = -0.65f + (frame == 7 ? 0 : (frame % 2 == 0 ? -0.03f : 0.03f));
                ASSERT_TRUE(submit(*native, renderer, instances));
                glfwWaitEventsTimeout(0.04);
            }
            auto image = WindowCapture::capture(*native);
            EXPECT_EQ(image.width, 800);
            EXPECT_EQ(image.height, 600);
            color(image, -0.510897f, 0.567851f, {188, 255, 188});
            color(image, 0.789103f, 0.567851f, {188, 255, 188});
            color(image, -0.186806f, 0.261438f, {188, 255, 188});
            color(image, -0.44f, -0.608333f, {255, 0, 0});
            color(image, 0.86f, -0.608333f, {255, 0, 0});
            WindowCapture::save(image, "review/model-instances-materials.ppm");

            // Align the glTF leaf with the original OBJ geometry. The near green surface must
            // occlude the red surface in both submission orders.
            const auto leaf = gltf->draws()[0].model;
            std::array overlap = {
                Model::ModelInstance{gltf, glm::translate(glm::mat4(1), glm::vec3(0, 0, 0.3f)) * glm::inverse(leaf)},
                Model::ModelInstance{obj, glm::translate(glm::mat4(1), glm::vec3(0, 0, -0.3f))}};
            for (int order = 0; order < 2; ++order)
            {
                ASSERT_TRUE(submit(*native, renderer, overlap));
                glfwWaitEventsTimeout(0.06);
                image = WindowCapture::capture(*native);
                color(image, 0.6f, -1.0f / 6, {188, 255, 188});
                std::reverse(overlap.begin(), overlap.end());
            }
            WindowCapture::save(image, "review/model-instances-depth.ppm");
            for (size_t count : {1u, 3u, 4u})
            {
                const auto input = std::span<const Model::ModelInstance>(instances).first(count);
                EXPECT_EQ(Model::makeModelDraws(input).size(), count == 1 ? 4u : count == 3 ? 10u : 12u);
                ASSERT_TRUE(submit(*native, renderer, input));
            }
            for (int frame = 0; frame < 3; ++frame)
                ASSERT_TRUE(submit(*native, renderer, {}, true));
            glfwWaitEventsTimeout(0.06);
            image = WindowCapture::capture(*native);
            color(image, 0.789103f, 0.567851f, {51, 61, 71});
            const auto ui = (static_cast<size_t>(24) * image.width + 24) * 4;
            EXPECT_GT(image.rgba[ui], 240);
            EXPECT_LT(image.rgba[ui + 1], 15);
            EXPECT_GT(image.rgba[ui + 2], 240);
            WindowCapture::save(image, "review/model-instances-empty-ui.ppm");
            glfwIconifyWindow(handle);
            for (int attempt = 0; attempt < 100 && !native->isIconified(); ++attempt)
            {
                native->pollEvents();
                glfwWaitEventsTimeout(0.005);
            }
            ASSERT_TRUE(native->isIconified());
            for (int frame = 0; frame < 3; ++frame)
                EXPECT_FALSE(submit(*native, renderer, instances));
            glfwRestoreWindow(handle);
            for (int attempt = 0; attempt < 100 && native->isIconified(); ++attempt)
            {
                native->pollEvents();
                glfwWaitEventsTimeout(0.005);
            }
            ASSERT_FALSE(native->isIconified());
            for (int frame = 0; frame < 3; ++frame)
                ASSERT_TRUE(submit(*native, renderer, instances));
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 4u);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded RGBA8 texture:"), 4u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 4u);
            EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline:"), 1u);
        }
        const auto expired = Model::makeModelDraws(instances);
        ASSERT_EQ(expired.size(), 12u);
        for (const auto& draw : expired)
        {
            EXPECT_FALSE(draw.mesh.valid());
            EXPECT_FALSE(draw.material.valid());
        }
        {
            Renderer next(*native, RendererValidationMode::Required);
            try
            {
                next.drawFrame({{800, 600}, camera({800, 600}), expired}, {0, 0, 0, 1});
                FAIL() << "Accepted expired draws";
            }
            catch (const Error::Exception& error)
            {
                EXPECT_EQ(error.code(), Error::Code::RENDER);
                EXPECT_NE(error.message().find("Mesh is invalid"), std::string::npos);
            }
            EXPECT_TRUE(submit(*native, next, {}, true));
        }
        cleanValidation();
    }
    TEST_F(ModelInstanceTest, DemoModelsWithUnicodeDependenciesPreserveDirectionAndExactGpuColor)
    {
        // Keep real relative dependency chains while renaming every input kind beyond ANSI.
        const std::string gltfPath = "models/场景😀-gltf/模型.gltf";
        const std::string objPath = "models/场景😀-obj/模型.obj";
        const std::string pngPath = "textures/方向😀.png";
        const auto read = [](std::string_view path) {
            std::ifstream input(Utils::Utf8::toFsPath(std::string("assets/") + std::string(path)), std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), {});
        };
        const auto write = [](std::string_view path, const std::string& contents) {
            const auto native = Utils::Utf8::toFsPath(std::string("assets/") + std::string(path));
            fs::create_directories(native.parent_path());
            std::ofstream(native, std::ios::binary).write(contents.data(), contents.size());
        };
        const auto replace = [](std::string& text, std::string_view from, std::string_view to) {
            const auto at = text.find(from);
            if (at == std::string::npos)
                throw std::runtime_error("Missing fixture dependency");
            text.replace(at, from.size(), to);
        };
        auto gltf = read(GLTF);
        replace(gltf, "geometry/mesh.bin", "几何😀/网格.bin");
        replace(gltf, "../../textures/direction_rgba.png", "../../textures/方向😀.png");
        write(gltfPath, gltf);
        write("models/场景😀-gltf/几何😀/网格.bin", read("models/minimal_gltf/geometry/mesh.bin"));
        auto obj = read(OBJ);
        replace(obj, "materials/model.mtl", "材质😀/颜色.mtl");
        write(objPath, obj);
        auto mtl = read("models/minimal_obj/materials/model.mtl");
        replace(mtl, "../../../textures/direction_rgba.png", "../../../textures/方向😀.png");
        write("models/场景😀-obj/材质😀/颜色.mtl", mtl);
        write(pngPath, read(PNG));

        auto native = window(true);
        // This test samples the composed desktop. Raise the window before presenting,
        // so capture's topmost change is not first applied immediately before BitBlt.
        ASSERT_TRUE(SetWindowPos(glfwGetWin32Window(static_cast<GLFWwindow*>(native->getNativeHandle())), HWND_TOPMOST,
                                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE));
        std::array<std::shared_ptr<const Model::PreparedModel>, 2> models;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            {
                Demo::SceneAssetPaths paths;
                paths.model = gltfPath;
                Demo::SceneAssets assets(paths);
                EXPECT_EQ(assets.texture, nullptr);
                renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
                models = {Model::prepareModel(renderer, gltfPath), Model::prepareModel(renderer, objPath)};
                assets.releaseCpuResources();
            }
            released(ResourceType::MODEL, gltfPath);
            released(ResourceType::MODEL, objPath);
            released(ResourceType::TEXTURE, pngPath);
            released(ResourceType::SHADER, "shaders/cube.vert.spv");
            released(ResourceType::SHADER, "shaders/cube.frag.spv");
            fileIO_.reset();

            const std::array<std::array<int, 4>, 12> direction = {{{255, 255, 255, 255},
                                                                   {255, 128, 0, 255},
                                                                   {0, 0, 0, 255},
                                                                   {0, 0, 255, 255},
                                                                   {128, 128, 128, 255},
                                                                   {255, 0, 0, 255},
                                                                   {0, 255, 0, 255},
                                                                   {255, 0, 255, 255},
                                                                   {0, 255, 255, 255},
                                                                   {17, 34, 51, 255},
                                                                   {255, 255, 0, 255},
                                                                   {128, 0, 255, 255}}};
            for (size_t format = 0; format < models.size(); ++format)
            {
                SCOPED_TRACE(format == 0 ? "glTF" : "OBJ");
                const auto& model = models[format];
                // Cancel the first source node only for the color chart view. The complete
                // cached hierarchy still renders, and separate tests assert its source matrices.
                const std::array instances{Model::ModelInstance{model, glm::inverse(model->draws()[0].model)}};
                const auto draws = Model::makeModelDraws(instances);
                std::vector<uint8_t> pixels;
                Detail::RendererReviewAccess::withFrame(
                    renderer, {{384, 256}, camera({384, 256}), draws},
                    [&](const auto& device, const auto& pipeline, auto frameSet, auto nativeDraws) {
                        pixels = TextureGpuReview::captureDraws(device, pipeline, frameSet, nativeDraws);
                    });
                ASSERT_EQ(pixels.size(), 384u * 256 * 4);
                const auto gpuColor = [&](float x, float y, const std::array<int, 4>& expected) {
                    const int px = static_cast<int>((x / 1.5f + 1) * 192);
                    const int py = static_cast<int>((1 - y) * 128);
                    for (size_t channel = 0; channel < 4; ++channel)
                        EXPECT_NEAR(pixels[(py * 384 + px) * 4 + channel], expected[channel], channel == 3 ? 0 : 1)
                            << "sample=" << x << ',' << y << " channel=" << channel;
                };
                for (int row = 0; row < 4; ++row)
                    for (int col = 0; col < 3; ++col)
                        gpuColor(-1 + (col + 0.5f) / 3, 0.5f - (row + 0.5f) / 4, direction[row * 3 + col]);
                gpuColor(0.6f, -1.0f / 6, {188, 255, 188, 255});
                const auto label = format == 0 ? "gltf" : "obj";
                WindowCapture::save({384, 256, pixels}, std::string("review/") + label + "-gpu.ppm");
                std::cout << label << ": GPU direction 12/12, linear 0.5 -> 188 +/- 1, alpha 255\n";

                for (int frame = 0; frame < 4; ++frame)
                {
                    ASSERT_TRUE(submit(*native, renderer, instances));
                    // Allow the composed desktop to catch up after window creation/resize.
                    glfwWaitEventsTimeout(0.04);
                }
                glfwWaitEventsTimeout(0.06);
                const auto displayed = WindowCapture::capture(*native);
                for (int row = 0; row < 4; ++row)
                    for (int col = 0; col < 3; ++col)
                    {
                        const auto& expected = direction[row * 3 + col];
                        color(displayed, -1 + (col + 0.5f) / 3, 0.5f - (row + 0.5f) / 4,
                              {expected[0], expected[1], expected[2]});
                    }
                color(displayed, 0.6f, -1.0f / 6, {188, 255, 188});
                WindowCapture::save(displayed, std::string("review/") + label + "-window.ppm");

                // Exercise the same initial controls and two shared instances used by Demo.
                glfwSetWindowSize(static_cast<GLFWwindow*>(native->getNativeHandle()), 1280, 720);
                auto scene = Demo::makeModelSceneControls();
                const std::array shared{Model::ModelInstance{model, Demo::makeSceneModel(scene.cube.transform)},
                                        Model::ModelInstance{model, Demo::makeSceneModel(scene.plane.transform)}};
                const auto sharedDraws = Model::makeModelDraws(shared);
                EXPECT_EQ(sharedDraws.size(), model->draws().size() * 2);
                for (int frame = 0; frame < 3; ++frame)
                {
                    native->pollEvents();
                    const auto extent = renderer.beginUiFrame();
                    ASSERT_TRUE(extent);
                    Demo::drawSceneControls(scene, format == 0 ? gltfPath : objPath, true);
                    renderer.imgui().endFrame();
                    auto* ui = renderer.imgui().drawData();
                    renderer.drawFrame(Demo::makeSceneFrame(*extent, sharedDraws, scene.camera), scene.clearColor,
                                       [&](void* commands) { renderer.imgui().recordDrawData(ui, commands); });
                    glfwWaitEventsTimeout(0.04);
                }
                glfwWaitEventsTimeout(0.06);
                WindowCapture::save(WindowCapture::capture(*native), std::string("review/") + label + "-demo.ppm");
            }
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 4u);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded RGBA8 texture:"), 4u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 4u);
            EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline:"), 1u);
        }
        for (const auto& model : models)
            for (const auto& draw : model->draws())
            {
                EXPECT_FALSE(draw.mesh.valid());
                EXPECT_FALSE(draw.material.valid());
            }
        cleanValidation();
    }
}  // namespace
