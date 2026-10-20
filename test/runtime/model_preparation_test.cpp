#include <GLFW/glfw3.h>
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <filesystem>
#include <fstream>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <iostream>
#include <sstream>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/model/prepared_model.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/platform/window/window.h"
#include "runtime/render/integration/imgui_renderer.h"
#include "runtime/render/renderer.h"
#include "runtime/resource/loader/model_loader.h"
#include "runtime/resource/loader/shader_loader.h"
#include "runtime/resource/loader/shader_resource.h"
#include "runtime/resource/loader/texture_loader.h"
#include "runtime/resource/loader/texture_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"
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
            for (const char* directory : {"models", "textures", "shaders"})
                fs::copy(previous_ / "assets" / directory, sandbox_ / "assets" / directory,
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
}  // namespace
