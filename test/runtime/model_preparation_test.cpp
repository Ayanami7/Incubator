#include <GLFW/glfw3.h>
#include <gtest/gtest.h>
#include <imgui.h>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <thread>
#include <type_traits>
#include <utility>

#include "../../examples/demo/model_cache.h"
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
#include "runtime/render/vulkan/vulkan_shadow_target.h"
#include "runtime/resource/loader/model_loader.h"
#include "runtime/resource/loader/shader_loader.h"
#include "runtime/resource/loader/shader_resource.h"
#include "runtime/resource/loader/texture_loader.h"
#include "runtime/resource/loader/texture_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"
#include "runtime/scene/scene.h"
#include "runtime/scene/scene_description.h"
#include "runtime/scene/scene_file.h"
#include "runtime/scene/scene_frame.h"
#include "runtime/scene/scene_rebuild.h"
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

    static_assert(
        std::is_same_v<decltype(std::declval<Model::PreparedModel&>().bounds()), const std::optional<Math::Aabb>&>);
    static_assert(
        std::is_same_v<decltype(std::declval<Model::PreparedModel&>().source()), const Model::ModelReference&>);

    TEST(ModelReferenceTest, NormalizesAndComparesEveryOptionAndRejectsInvalidValues)
    {
        const Model::ModelReference base("Models\\Unused\\..\\Scene.GLTF");
        EXPECT_EQ(base.path(), "models/scene.gltf");
        EXPECT_EQ(base, Model::ModelReference("models/./scene.gltf"));
        EXPECT_NE(base, Model::ModelReference("models/other.gltf"));
        Model::PrepareOptions options;
        options.mipMode = TextureMipMode::FullChain;
        EXPECT_NE(base, Model::ModelReference("models/scene.gltf", options));
        options = {};
        options.sampler.minFilter = TextureFilter::Linear;
        EXPECT_NE(base, Model::ModelReference("models/scene.gltf", options));
        options = {};
        options.sampler.magFilter = TextureFilter::Linear;
        EXPECT_NE(base, Model::ModelReference("models/scene.gltf", options));
        for (const auto mip : {MipFilter::Nearest, MipFilter::Linear})
        {
            options = {};
            options.sampler.mipFilter = mip;
            EXPECT_NE(base, Model::ModelReference("models/scene.gltf", options));
        }
        options = {};
        options.shading = ShadingMode::Lambert;
        EXPECT_NE(base, Model::ModelReference("models/scene.gltf", options));
        for (const auto path : {std::string(""), std::string("../escape.gltf"), std::string("C:asset.gltf"),
                                std::string("x\0y", 3), std::string("\xff", 1)})
            EXPECT_THROW((void)Model::ModelReference{path}, Error::Exception);
        options = {};
        options.mipMode = static_cast<TextureMipMode>(99);
        EXPECT_THROW((void)(Model::ModelReference{"models/scene.gltf", options}), Error::Exception);
        options = {};
        options.sampler.minFilter = static_cast<TextureFilter>(99);
        EXPECT_THROW((void)(Model::ModelReference{"models/scene.gltf", options}), Error::Exception);
        options = {};
        options.sampler.magFilter = static_cast<TextureFilter>(99);
        EXPECT_THROW((void)(Model::ModelReference{"models/scene.gltf", options}), Error::Exception);
        options = {};
        options.sampler.mipFilter = static_cast<MipFilter>(99);
        EXPECT_THROW((void)(Model::ModelReference{"models/scene.gltf", options}), Error::Exception);
        options = {};
        options.shading = static_cast<ShadingMode>(99);
        EXPECT_THROW((void)(Model::ModelReference{"models/scene.gltf", options}), Error::Exception);
    }

    size_t occurrences(std::string_view text, std::string_view value)
    {
        size_t count = 0;
        for (auto at = text.find(value); at != std::string_view::npos; at = text.find(value, at + value.size()))
            ++count;
        return count;
    }

    float readShadowDepth(const Vulkan::VulkanDevice& device, const Vulkan::VulkanImage& image, uint32_t x = 512,
                          uint32_t y = 512)
    {
        const auto bytes = TextureGpuReview::captureMip(device, image, 0, {1, 1}, VK_IMAGE_ASPECT_DEPTH_BIT,
                                                        {int32_t(x), int32_t(y), 0});
        if (image.format() == VK_FORMAT_D16_UNORM)
        {
            uint16_t value;
            std::memcpy(&value, bytes.data(), sizeof(value));
            return float(value) / 65535.0f;
        }
        float value;
        std::memcpy(&value, bytes.data(), sizeof(value));
        return value;
    }

    uint8_t sampleShadowDepth(const Vulkan::VulkanDevice& device, const Vulkan::VulkanImage& image,
                              const std::vector<uint32_t>& vertex, const std::vector<uint32_t>& fragment)
    {
        // 复用正式纹理 Shader 的 Unlit 路径验证真实片元采样，只取深度的 R 分量。
        const auto frame = Vulkan::makeFrameUniform({{384, 256}, {glm::mat4(1), glm::mat4(1)}, {}});
        const std::array uniforms{std::make_shared<Vulkan::VulkanBuffer>(
            device, sizeof(frame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        uniforms[0]->write(0, std::as_bytes(std::span(&frame, 1)));
        Vulkan::VulkanBindingSet bindings(device, uniforms);
        Vulkan::VulkanSampler sampler(device, {});
        Vulkan::VulkanMaterialBinding material(device, bindings.materialLayout(), image, sampler, {1, 1, 1});
        Vulkan::VulkanPipeline pipeline(device, VK_FORMAT_R8G8B8A8_UNORM, device.findDepthFormat(), bindings.layout(),
                                        bindings.materialLayout(), vertex, fragment);
        auto mesh = Demo::makePlaneMesh();
        for (auto& v : mesh.vertices)
        {
            v.uv[0] = 0.5f;
            v.uv[1] = 0.5f;
        }
        auto vertices = Vulkan::VulkanBuffer::uploadStatic(
            device, std::as_bytes(std::span(mesh.vertices)), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
        auto indices = Vulkan::VulkanBuffer::uploadStatic(
            device, std::as_bytes(std::span(mesh.indices)), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT);
        const std::array draws{Vulkan::VulkanDrawItem{vertices->handle(),
                                                      indices->handle(),
                                                      uint32_t(mesh.indices.size()),
                                                      material.descriptorSet(),
                                                      {glm::mat4(1), {}}}};
        const auto pixels = TextureGpuReview::captureDraws(device, pipeline, bindings.descriptorSet(0), draws);
        return pixels[(128 * 384 + 192) * 4];
    }

    Scene::Camera shadowReviewCamera(glm::vec3 eye = {0, -3, 3})
    {
        const auto view = glm::lookAtRH(eye, glm::vec3(0, 0.5f, 0), glm::vec3(0, 1, 0));
        return Scene::Camera(eye, glm::quat_cast(glm::mat3(glm::inverse(view))));
    }

    void expectShadowPixel(const Scene::SceneFrame& frame, std::span<const uint8_t> pixels, glm::vec3 world,
                           glm::vec3 linear)
    {
        ASSERT_EQ(pixels.size(), size_t(frame.extent.width) * frame.extent.height * 4);
        const auto clip = frame.camera.projection * frame.camera.view * glm::vec4(world, 1);
        ASSERT_GT(clip.w, 0);
        const auto ndc = glm::vec3(clip) / clip.w;
        const auto x = int((ndc.x * 0.5f + 0.5f) * frame.extent.width);
        const auto y = int((ndc.y * 0.5f + 0.5f) * frame.extent.height);
        ASSERT_GE(x, 0);
        ASSERT_LT(x, int(frame.extent.width));
        ASSERT_GE(y, 0);
        ASSERT_LT(y, int(frame.extent.height));
        const auto offset = (size_t(y) * frame.extent.width + size_t(x)) * 4;
        for (int channel = 0; channel < 3; ++channel)
        {
            const double value = std::clamp(double(linear[channel]), 0.0, 1.0);
            const double encoded = value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
            EXPECT_NEAR(pixels[offset + channel], encoded * 255, 2) << "channel=" << channel;
        }
        EXPECT_EQ(pixels[offset + 3], 255);
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
            for (const char* directory : {"models", "textures", "shaders", "fonts"})
                fs::copy(fixtureRoot / directory, sandbox_ / "assets" / directory, fs::copy_options::recursive);
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
            source_ = std::make_unique<AssetSource>(*fileIO_);
            auto& manager = ResourceManager::instance();
            manager.registerLoader(ResourceType::MODEL, std::make_unique<ModelLoader>(*source_));
            manager.registerLoader(ResourceType::TEXTURE, std::make_unique<TextureLoader>(*source_));
            manager.registerLoader(ResourceType::SHADER, std::make_unique<ShaderLoader>(*source_));
        }
        void TearDown() override
        {
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            source_.reset();
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
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        }
        void shadowShader(Renderer& renderer)
        {
            auto& manager = ResourceManager::instance();
            const auto id = manager.registerResource(ResourceType::SHADER, "shaders/shadow.vert.spv");
            const auto* shader = manager.get<ShaderResource>(id);
            ASSERT_NE(shader, nullptr);
            renderer.prepareShadowResources(*shader);
            manager.release(id);
            released(ResourceType::SHADER, "shaders/shadow.vert.spv");
        }
        std::vector<uint8_t> shadowScenePixels(Window::Window& native, Renderer& renderer,
                                               const Scene::SceneFrame& frame)
        {
            native.pollEvents();
            const auto extent = renderer.beginUiFrame();
            EXPECT_TRUE(extent);
            if (!extent)
                return {};
            const auto output = renderer.prepareSceneTarget(frame.extent);
            renderer.imgui().endFrame();
            renderer.drawSceneTarget(output, frame.input(), {0.12f, 0.16f, 0.22f, 1});
            std::vector<uint8_t> pixels;
            Detail::RendererReviewAccess::withSceneOutput(renderer, output, [&](const auto& device, const auto& image) {
                pixels = TextureGpuReview::captureMip(device, image, 0, {frame.extent.width, frame.extent.height});
            });
            return pixels;
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
        std::unique_ptr<AssetSource> source_;
        std::ostringstream messages_, resourceMessages_;
    };

    TEST_F(ModelPreparationTest, SpecifiedAssetRootPreparesBothFormatsWithoutFallbackToDefaultDirectory)
    {
        const auto root = sandbox_ / Utils::Utf8::toFsPath("独立 资产根");
        fs::rename(sandbox_ / "assets", root);
        fs::create_directories(sandbox_ / "assets");
        fs::copy(root / "fonts", sandbox_ / "assets/fonts", fs::copy_options::recursive);
        auto& manager = ResourceManager::instance();
        manager.reset();
        source_ = std::make_unique<AssetSource>(*fileIO_, root);
        manager.bindAssetSource(source_.get());
        manager.registerLoader(ResourceType::MODEL, std::make_unique<ModelLoader>(*source_));
        manager.registerLoader(ResourceType::TEXTURE, std::make_unique<TextureLoader>(*source_));
        manager.registerLoader(ResourceType::SHADER, std::make_unique<ShaderLoader>(*source_));
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            const auto gltf = Model::prepareModel(renderer, GLTF);
            const auto obj = Model::prepareModel(renderer, OBJ);
            ASSERT_FALSE(gltf->textures().empty());
            ASSERT_FALSE(obj->textures().empty());
            EXPECT_EQ(gltf->source().path(), GLTF);
            EXPECT_EQ(obj->source().path(), OBJ);
            draw(*native, renderer, *gltf);
            draw(*native, renderer, *obj);
            fs::remove(root / "models/minimal_gltf/geometry/mesh.bin");
            EXPECT_THROW(Model::prepareModel(renderer, GLTF), Error::Exception);
            fs::remove(root / "models/minimal_obj/materials/model.mtl");
            EXPECT_THROW(Model::prepareModel(renderer, OBJ), Error::Exception);
        }
        EXPECT_EQ(fs::current_path(), sandbox_);
        EXPECT_FALSE(fs::exists(sandbox_ / "assets/models"));
        EXPECT_FALSE(fs::exists(sandbox_ / "assets/textures"));
        EXPECT_FALSE(fs::exists(sandbox_ / "assets/shaders"));
        manager.reset();
        manager.bindAssetSource(nullptr);
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, ShadowSamplingMatchesLightingAndPreservesUnlitAcrossEnableChanges)
    {
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            Model::PrepareOptions options;
            options.shading = ShadingMode::Lambert;
            const auto receiver = Model::prepareModel(renderer, "models/shadows/receiver.obj", options);
            const auto caster = Model::prepareModel(renderer, "models/shadows/caster.obj");
            EXPECT_EQ(caster->source().options().shading, ShadingMode::Unlit);
            ASSERT_FALSE(receiver->textures().empty());
            const auto unlit = renderer.createMaterial(MaterialDesc{receiver->textures()[0]});
            Scene::Scene scene;
            scene.createEntity(receiver);
            const auto casterId = scene.createEntity(caster, Scene::Transform({0, 0.75f, 5}));
            ShadowSettings settings;
            std::vector<uint8_t> baseline;
            for (int i = 0; i < 13; ++i)
            {
                SCOPED_TRACE(i);
                if (i == 1)
                    shadowShader(renderer);
                scene.setCamera(shadowReviewCamera(i == 10 ? glm::vec3(0.25f, -3, 3) : glm::vec3(0, -3, 3)));
                Lighting lighting{{0, 0, -1}, {0.6f, 0.7f, 0.8f}, 0.8f, 0.2f};
                if (i == 6)
                    lighting.direction = {0, -1, -1};
                if (i == 8 || i == 9)
                    lighting.intensity = 0;
                if (i == 9)
                    lighting.ambient = 0;
                scene.setLighting(lighting);
                scene.setTransform(casterId, Scene::Transform({i == 5 ? 3.0f : 0.0f, 0.75f, 5}));
                settings.enabled = i != 0 && i != 3 && i != 12;
                auto frame = Scene::extractSceneFrame(scene, {384, 256}, true, settings);
                ASSERT_TRUE(frame);
                // 投射物完全在主相机外，但它仍应遮挡画面内的接收面。
                ASSERT_EQ(frame->draws.size(), receiver->draws().size());
                if (frame->shadow)
                    EXPECT_GT(frame->shadowDraws.size(), frame->draws.size());
                if (i == 2 || i == 11)
                    frame->shadowDraws.clear();
                if (i == 7)
                    for (auto& draw : frame->draws)
                        draw.material = unlit;
                const auto pixels = shadowScenePixels(*native, renderer, *frame);
                const auto diffuse = std::max(glm::normalize(-lighting.direction).z, 0.0f);
                const auto lit = glm::vec3(lighting.ambient) + lighting.color * lighting.intensity * diffuse;
                const bool occluded = i == 1 || i == 4 || i == 10;
                expectShadowPixel(*frame, pixels, {0, 0.75f, 0},
                                  i == 7     ? glm::vec3(1)
                                  : occluded ? glm::vec3(lighting.ambient)
                                             : lit);
                expectShadowPixel(*frame, pixels, {0, -0.75f, 0}, i == 7 ? glm::vec3(1) : lit);
                if (i == 0)
                {
                    baseline = pixels;
                    const auto direct = TextureGpuReview::captureFrame(renderer, frame->input());
                    expectShadowPixel(*frame, direct, {0, 0.75f, 0}, lit);
                }
                if (i == 12)
                    EXPECT_EQ(pixels, baseline);
                size_t clearPixels = 0;
                for (size_t at = 0; at + 3 < pixels.size(); at += 4)
                    if (std::abs(int(pixels[at]) - 31) <= 1 && std::abs(int(pixels[at + 1]) - 41) <= 1 &&
                        std::abs(int(pixels[at + 2]) - 56) <= 1)
                        ++clearPixels;
                EXPECT_GT(clearPixels, 0u);
            }
            for (int i = 0; i < 4; ++i)
            {
                native->pollEvents();
                ASSERT_TRUE(renderer.beginUiFrame());
                const FrameExtent extent{i < 2 ? 384u : 256u, 256};
                const auto output = renderer.prepareSceneTarget(extent);
                settings.enabled = i != 2;
                const auto frame = Scene::extractSceneFrame(scene, extent, true, settings);
                ASSERT_TRUE(frame);
                auto* ui = ImGui::GetBackgroundDrawList();
                ui->AddImage(renderer.sceneImage(output), {0, 0}, {256, 256});
                ui->AddRectFilled({8, 8}, {64, 64}, IM_COL32(255, 0, 255, 255));
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(output, frame->input(), {0.12f, 0.16f, 0.22f, 1}, [&](void* commands) {
                    renderer.imgui().recordDrawData(renderer.imgui().drawData(),
                                                    static_cast<VkCommandBuffer>(commands));
                });
            }
            // 不等待回读即关闭，覆盖阴影采样、视口重建、UI 合成和在途描述符的共同持有期。
        }
        EXPECT_EQ(occurrences(messages_.str(), "Created shadow target:"), 2u);
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, ShadowSamplingTreatsLightBoundsAsLitAndUsesReceiverDepthBias)
    {
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            shadowShader(renderer);
            Model::PrepareOptions options;
            options.shading = ShadingMode::Lambert;
            const auto receiver = Model::prepareModel(renderer, "models/shadows/receiver.obj", options);
            Scene::Scene scene;
            scene.createEntity(receiver);
            scene.setCamera(shadowReviewCamera());
            const Lighting lighting{{0, 0, -1}, {0.6f, 0.7f, 0.8f}, 0.8f, 0.2f};
            scene.setLighting(lighting);
            struct Sample
            {
                glm::vec3 center;
                glm::vec2 halfExtent;
                float nearPlane, farPlane, casterZ, bias;
                bool occluded;
            };
            const std::array<Sample, 9> samples{{{{-4, 0.75f, 0}, {1, 1}, 1, 9, 2, 0.001f, false},
                                                 {{4, 0.75f, 0}, {1, 1}, 1, 9, 2, 0.001f, false},
                                                 {{0, 4.75f, 0}, {1, 1}, 1, 9, 2, 0.001f, false},
                                                 {{0, -3.25f, 0}, {1, 1}, 1, 9, 2, 0.001f, false},
                                                 {{0, 0.75f, 6}, {1, 1}, 1, 3, 6.5f, 0.001f, false},
                                                 {{0, 0.75f, -6}, {1, 1}, 1, 3, -5.5f, 0.001f, false},
                                                 {{0, 0, 0}, {4, 4}, 1, 9, 0.004f, 0, true},
                                                 {{0, 0, 0}, {4, 4}, 1, 9, 0.004f, 0.00025f, true},
                                                 {{0, 0, 0}, {4, 4}, 1, 9, 0.004f, 0.001f, false}}};
            for (size_t i = 0; i < samples.size(); ++i)
            {
                SCOPED_TRACE(i);
                const auto& sample = samples[i];
                const ShadowSettings settings{
                    true, sample.center, sample.halfExtent, sample.nearPlane, sample.farPlane, sample.bias};
                auto frame = Scene::extractSceneFrame(scene, {384, 256}, true, settings);
                ASSERT_TRUE(frame);
                // 投射平面覆盖整张深度图，范围外若误用 ClampToEdge 会出现可判定的暗区。
                const auto model =
                    glm::translate(glm::mat4(1), glm::vec3(sample.center.x, sample.center.y, sample.casterZ)) *
                    glm::scale(glm::mat4(1), glm::vec3(4, 4, 1));
                frame->shadowDraws = {{receiver->meshes()[0], {}, model}};
                const auto pixels = shadowScenePixels(*native, renderer, *frame);
                const auto expected = sample.occluded
                                          ? glm::vec3(lighting.ambient)
                                          : glm::vec3(lighting.ambient) + lighting.color * lighting.intensity;
                expectShadowPixel(*frame, pixels, {0, 0.75f, 0}, expected);
            }
        }
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, ShadowDepthTargetsFollowSlotsAndKeepNearestSurfaceWithoutMaterials)
    {
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            shadowShader(renderer);
            std::array<std::vector<uint32_t>, 2> words;
            auto& manager = ResourceManager::instance();
            for (size_t i = 0; i < words.size(); ++i)
            {
                const auto id = manager.registerResource(ResourceType::SHADER,
                                                         i ? "shaders/cube.frag.spv" : "shaders/cube.vert.spv");
                words[i] = manager.get<ShaderResource>(id)->words();
                manager.release(id);
            }
            const std::array<MeshVertex, 4> vertices{
                {{{-2, -2, 0}, {0, 0}}, {{2, -2, 0}, {1, 0}}, {{2, 2, 0}, {1, 1}}, {{-2, 2, 0}, {0, 1}}}};
            const std::array<uint32_t, 6> forward{0, 1, 2, 0, 2, 3}, reverse{2, 1, 0, 3, 2, 0};
            const auto mesh = renderer.createMesh(vertices, forward);
            const auto reversed = renderer.createMesh(vertices, reverse);
            ShadowSettings settings;
            settings.enabled = true;
            settings.halfExtent = {4, 4};
            settings.nearPlane = 1;
            settings.farPlane = 9;
            const auto shadow = makeShadowFrame(settings, {0, 0, -1});
            const std::array<float, 8> expected{0.25f, 0.75f, 1, 0.125f, 1, 1, 0.25f, 0.75f};
            std::array<VkImage, 2> handles{};
            std::array<SceneOutput, 2> outputs;
            for (size_t i = 0; i < expected.size(); ++i)
            {
                native->pollEvents();
                ASSERT_TRUE(renderer.beginUiFrame());
                const FrameExtent extent{i < 4 ? 192u : 256u, 128};
                auto output = renderer.prepareSceneTarget(extent);
                std::vector<DrawItem> casters;
                if (i != 2 && i != 4 && i != 5)
                {
                    casters.push_back({mesh, {}, glm::translate(glm::mat4(1), {0, 0, -2})});
                    if (i != 1 && i != 7)
                    {
                        const auto model = glm::translate(glm::mat4(1), {0, 0, i == 3 ? 3 : 2}) *
                                           glm::scale(glm::mat4(1), {-0.5f, 1.5f, 0});
                        casters.push_back({i == 3 ? reversed : mesh, {}, model});
                        casters.push_back(casters[0]);
                    }
                }
                FrameInput input{extent, {glm::mat4(1), glm::mat4(1)}, {}, {}, i == 4 ? std::nullopt : shadow, casters};
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(output, input, {0, 0, 0, 1});
                outputs[i % 2] = output;
                if (i == 4)
                    continue;
                Detail::RendererReviewAccess::withShadowOutput(
                    renderer, output, [&](const auto& device, const auto& image) {
                        EXPECT_EQ(image.extent().width, 1024u);
                        EXPECT_EQ(image.extent().height, 1024u);
                        EXPECT_EQ(image.mipLevels(), 1u);
                        if (!handles[i % 2])
                            handles[i % 2] = image.handle();
                        EXPECT_EQ(image.handle(), handles[i % 2]);
                        EXPECT_NEAR(readShadowDepth(device, image), expected[i], 0.00003f);
                        EXPECT_NEAR(readShadowDepth(device, image, 0, 0), 1, 0.00003f);
                        if (i < 2)
                        {
                            const double encoded = 1.055 * std::pow(double(expected[i]), 1.0 / 2.4) - 0.055;
                            EXPECT_NEAR(sampleShadowDepth(device, image, words[0], words[1]), encoded * 255, 2);
                        }
                    });
                if (i == 1)
                    Detail::RendererReviewAccess::withShadowOutput(
                        renderer, outputs[0], [&](const auto& device, const auto& image) {
                            EXPECT_NE(handles[0], handles[1]);
                            EXPECT_NEAR(readShadowDepth(device, image), 0.25f, 0.00003f);
                        });
            }
        }
        EXPECT_EQ(occurrences(messages_.str(), "Created shadow target:"), 2u);
        EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline: color format 0,"), 1u);
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, ShadowResourcesRejectMissingShaderAndInvalidMeshBeforeSubmission)
    {
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            ShaderResource empty({}, {});
            EXPECT_THROW(renderer.prepareShadowResources(empty), Error::Exception);
            shaders(renderer);
            EXPECT_THROW(renderer.prepareShadowResources(empty), Error::Exception);
            native->pollEvents();
            ASSERT_TRUE(renderer.beginUiFrame());
            auto output = renderer.prepareSceneTarget({128, 128});
            FrameInput input{{128, 128}, {glm::mat4(1), glm::mat4(1)}, {}, {}, ShadowFrame{}, {}};
            renderer.imgui().endFrame();
            bool recorded = false;
            const auto reject = [&](auto&& submit) {
                try
                {
                    submit();
                    FAIL() << "Invalid shadow submission was accepted";
                }
                catch (const Error::Exception& error)
                {
                    EXPECT_EQ(error.code(), Error::Code::RENDER);
                }
                EXPECT_FALSE(recorded);
            };
            reject([&] { renderer.drawSceneTarget(output, input, {0, 0, 0, 1}, [&](void*) { recorded = true; }); });
            shadowShader(renderer);
            const std::array<DrawItem, 1> invalid{DrawItem{}};
            input.shadowDraws = invalid;
            reject([&] { renderer.drawSceneTarget(output, input, {0, 0, 0, 1}, [&](void*) { recorded = true; }); });
            reject([&] { renderer.drawFrame(input, {0, 0, 0, 1}, [&](void*) { recorded = true; }); });
            input.shadow.reset();
            EXPECT_NO_THROW(renderer.drawSceneTarget(output, input, {0, 0, 0, 1}));
        }
        EXPECT_EQ(occurrences(messages_.str(), "Created shadow target:"), 0u);
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, IndependentSceneTargetsFollowFrameSlotsAndExpireAfterResize)
    {
        auto native = window();
        SceneOutput retained;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            const auto model = Model::prepareModel(renderer, GLTF);
            EXPECT_FALSE(renderer.prepareSceneTarget({0, 100}).valid());
            for (uint32_t i = 0; i < 10; ++i)
            {
                native->pollEvents();
                const auto windowExtent = renderer.beginUiFrame();
                ASSERT_TRUE(windowExtent);
                const FrameExtent extent{i < 4 ? 192u : 256u, 128};
                auto output = renderer.prepareSceneTarget(extent);
                ASSERT_TRUE(output.valid());
                EXPECT_EQ(output.extent(), extent);
                if (i == 0)
                    retained = output;
                if (i == 4)
                    EXPECT_FALSE(retained.valid());
                Scene::Scene scene;
                scene.setCamera(Scene::Camera({0, 0, 3}));
                if (i % 2)
                    scene.createEntity(model);
                const auto frame = Scene::extractSceneFrame(scene, extent);
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(output, frame->input(), {0.2f, 0.3f, 0.4f, 1});
                if (i == 9)
                    retained = output;
                Detail::RendererReviewAccess::withSceneOutput(
                    renderer, output, [&](const auto& device, const auto& image) {
                        const auto pixels =
                            TextureGpuReview::captureMip(device, image, 0, {extent.width, extent.height});
                        EXPECT_NEAR(pixels[0], 51, 1);
                        EXPECT_NEAR(pixels[1], 77, 1);
                        EXPECT_NEAR(pixels[2], 102, 1);
                        if (i % 2)
                            EXPECT_TRUE(std::any_of(pixels.begin(), pixels.end(), [](auto value) {
                                return value != 51 && value != 77 && value != 102 && value != 255;
                            }));
                    });
            }
        }
        EXPECT_FALSE(retained.valid());
        EXPECT_EQ(occurrences(messages_.str(), "Scene target created:"), 2u);
        EXPECT_EQ(occurrences(messages_.str(), "Scene target resized:"), 2u);
        EXPECT_NE(messages_.str().find("192x128 -> 256x128, color format"), std::string::npos);
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, LambertPreparationKeepsImportedUnlitAndSharesRepeatedGeometryAndWhite)
    {
        constexpr const char* path = "models/normals/folded.gltf";
        const auto sourcePath = fs::path("assets") / path;
        std::string source;
        {
            std::ifstream stream(sourcePath, std::ios::binary);
            source.assign(std::istreambuf_iterator<char>(stream), {});
        }
        const std::string primitive =
            R"({"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2, "material": 0})";
        const auto at = source.find(primitive);
        ASSERT_NE(at, std::string::npos);
        auto ordinary = primitive;
        const std::string material = "\"material\": 0";
        ordinary.replace(ordinary.find(material), material.size(), "\"material\": 1");
        source.replace(at, primitive.size(), primitive + "," + ordinary);
        const std::string node = R"({"name": "folded", "mesh": 0})";
        const auto nodeAt = source.find(node);
        ASSERT_NE(nodeAt, std::string::npos);
        source.replace(nodeAt, node.size(), R"({"name":"folded","mesh":0,"children":[1]},{"name":"shared","mesh":0})");
        {
            std::ofstream stream(sourcePath, std::ios::binary | std::ios::trunc);
            stream << source;
        }
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            Model::PrepareOptions options;
            options.shading = static_cast<ShadingMode>(99);
            EXPECT_THROW(Model::prepareModel(renderer, path, options), Error::Exception);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 0u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 0u);
            const auto legacy = Model::prepareModel(renderer, path);
            EXPECT_EQ(legacy->source(), Model::ModelReference(path));
            options.shading = ShadingMode::Lambert;
            std::string sourcePath = "Models/normals/../normals/FOLDED.gltf";
            const auto lambert = Model::prepareModel(renderer, sourcePath, options);
            sourcePath.clear();
            options.shading = ShadingMode::Unlit;
            EXPECT_EQ(lambert->source().path(), path);
            EXPECT_EQ(lambert->source().options().shading, ShadingMode::Lambert);
            released(ResourceType::MODEL, path);
            source_.reset();
            fileIO_.reset();
            for (const auto& prepared : {legacy, lambert})
            {
                EXPECT_EQ(prepared->meshes().size(), 2u);
                EXPECT_EQ(prepared->materials().size(), 2u);
                EXPECT_EQ(prepared->textures().size(), 1u);
                ASSERT_EQ(prepared->draws().size(), 4u);
                std::array draws{prepared->draws()[0], prepared->draws()[1]};
                draws[0].model = glm::translate(glm::mat4(1), glm::vec3(-0.9f, -0.3f, 0.2f)) *
                                 glm::scale(glm::mat4(1), glm::vec3(0.8f));
                draws[1].model = glm::translate(glm::mat4(1), glm::vec3(0.1f, -0.3f, 0.2f)) *
                                 glm::scale(glm::mat4(1), glm::vec3(0.8f));
                const FrameInput frame{{384, 256}, {glm::mat4(1), glm::mat4(1)}, draws, {{0, 0, -1}, {1, 1, 1}, 0, 0}};
                const auto pixels = TextureGpuReview::captureFrame(renderer, frame);
                ASSERT_EQ(pixels.size(), 384u * 256 * 4);
                for (int channel = 0; channel < 3; ++channel)
                {
                    EXPECT_NEAR(pixels[(115 * 384 + 57) * 4 + channel], 255, 2);
                    EXPECT_NEAR(pixels[(115 * 384 + 249) * 4 + channel], prepared == lambert ? 0 : 255, 2);
                }
                EXPECT_EQ(pixels[(115 * 384 + 57) * 4 + 3], 255);
                EXPECT_EQ(pixels[(115 * 384 + 249) * 4 + 3], 255);
            }
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 4u);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
            EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline:"), 1u);
        }
        cleanValidation();
    }

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
                ASSERT_TRUE(model->bounds());
                EXPECT_EQ(&model->bounds(), &shared->bounds());
                const auto bounds = *model->bounds();
                model.reset();
                EXPECT_FALSE(shared->nodes().empty());
                ASSERT_TRUE(shared->bounds());
                EXPECT_EQ(shared->bounds()->min, bounds.min);
                EXPECT_EQ(shared->bounds()->max, bounds.max);
                for (int frame = 0; frame < 3; ++frame)
                    draw(*native, renderer, *shared, frame % 2);
                models.push_back(shared);
            }
            EXPECT_EQ(models[0]->nodes()[models[0]->rootNode()].name, "SourceRoot");
            EXPECT_EQ(occurrences(messages_.str(), "Prepared model '"), 2u);
            EXPECT_NE(messages_.str().find("2 meshes, 2 materials, 2 textures; requested mip=baseLevel"),
                      std::string::npos);
            EXPECT_FLOAT_EQ(models[0]->nodes()[models[0]->rootNode()].localTransform[3][0], 0.25f);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 4u);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 4u);
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
                Scene::Scene comparison;
                comparison.setCamera(Scene::Camera({0, 0, 6}));
                const float displayScale = index == 0 ? 0.075f : 24.0f;
                const float displayY = index == 0 ? 0.0f : -0.78f;
                comparison.createEntity(models.back(),
                                        Scene::Transform({0, displayY, 0}, {1, 0, 0, 0}, glm::vec3(displayScale)));
                comparison.createEntity(models.back(),
                                        Scene::Transform({1000, displayY, 0}, {1, 0, 0, 0}, glm::vec3(displayScale)));
                const auto enabled = Scene::extractSceneFrame(comparison, {384, 256});
                const auto disabled = Scene::extractSceneFrame(comparison, {384, 256}, false);
                ASSERT_TRUE(enabled);
                ASSERT_TRUE(disabled);
                EXPECT_EQ(enabled->draws.size(), models.back()->draws().size());
                EXPECT_EQ(disabled->draws.size(), models.back()->draws().size() * 2);
                const auto capture = [&](const Scene::SceneFrame& frame) {
                    std::vector<uint8_t> pixels;
                    Detail::RendererReviewAccess::withFrame(
                        renderer, frame.input(),
                        [&](const auto& device, const auto& pipeline, auto frameSet, auto draws) {
                            pixels = TextureGpuReview::captureDraws(device, pipeline, frameSet, draws);
                        });
                    return pixels;
                };
                const auto enabledPixels = capture(*enabled);
                const auto disabledPixels = capture(*disabled);
                ASSERT_EQ(enabledPixels.size(), 384u * 256 * 4);
                EXPECT_TRUE(enabledPixels == disabledPixels);
                auto blank = *enabled;
                blank.draws.clear();
                EXPECT_TRUE(enabledPixels != capture(blank));
                const auto label = index == 0 ? "teapot" : "avocado";
                WindowCapture::save({384, 256, enabledPixels},
                                    fs::path("review") / (std::string(label) + "-culled.ppm"));
                WindowCapture::save({384, 256, disabledPixels},
                                    fs::path("review") / (std::string(label) + "-unculled.ppm"));
                std::cout << label << ": culled=" << enabled->draws.size() << " unculled=" << disabled->draws.size()
                          << " identical nonempty GPU image\n";
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
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), expectedTextures);
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
            Scene::Scene scene;
            const auto hidden =
                scene.createEntity(model, Scene::Transform({1000, 0, 0}, {1, 0, 0, 0}, {-2, 0, 3}), false);
            scene.createEntity();
            const auto snapshot = Scene::captureScene(scene, {0.2f, 0.3f, 0.4f, 1});
            ASSERT_EQ(snapshot.entities.size(), 2u);
            ASSERT_TRUE(snapshot.entities[0].model);
            EXPECT_EQ(*snapshot.entities[0].model, model->source());
            EXPECT_FALSE(snapshot.entities[0].visible);
            EXPECT_EQ(snapshot.entities[0].transform.position(), glm::vec3(1000, 0, 0));
            EXPECT_FALSE(snapshot.entities[1].model);
            scene.setModel(hidden, {});
            scene.clear();
            EXPECT_EQ(snapshot.entities[0].model->path(), "models/shared.gltf");
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
            ASSERT_TRUE(model->bounds());
            EXPECT_EQ(model->bounds()->min, glm::vec3(-0.8f, -0.6f, 0));
            EXPECT_EQ(model->bounds()->max, glm::vec3(0.8f, 0.8f, 0));
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
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 4u);
            EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline:"), 1u);
        }
        EXPECT_FALSE(model->meshes()[0].valid());
        EXPECT_FALSE(model->textures()[0].valid());
        EXPECT_FALSE(model->textures()[1].valid());
        EXPECT_FALSE(model->materials()[2].valid());
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, ExplicitSamplingPreservesSharingMetadataAndDefaultWhileUsingMipsOnGpu)
    {
        class BlackWhiteLoader final : public LoaderBase
        {
        public:
            std::unique_ptr<ResourceObject> load(const ResourceId& id, std::string_view) override
            {
                return std::make_unique<TextureResource>(
                    id, 2, 2, std::vector<uint8_t>{0, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 0, 255, 255, 255, 255});
            }
        };
        auto& manager = ResourceManager::instance();
        manager.registerLoader(ResourceType::MODEL, std::make_unique<SharedModelLoader>());
        manager.registerLoader(ResourceType::TEXTURE, std::make_unique<BlackWhiteLoader>());
        auto native = window();
        std::vector<std::shared_ptr<const Model::PreparedModel>> models;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            const std::array options{
                Model::PrepareOptions{},
                Model::PrepareOptions{TextureMipMode::FullChain,
                                      {TextureFilter::Linear, TextureFilter::Linear, MipFilter::BaseLevel}},
                Model::PrepareOptions{TextureMipMode::FullChain,
                                      {TextureFilter::Linear, TextureFilter::Linear, MipFilter::Linear}}};
            for (size_t policy = 0; policy < options.size(); ++policy)
            {
                SCOPED_TRACE(policy);
                const auto model = policy == 0 ? Model::prepareModel(renderer, "models/shared.gltf")
                                               : Model::prepareModel(renderer, "models/shared.gltf", options[policy]);
                EXPECT_EQ(model->source(), Model::ModelReference("models/shared.gltf", options[policy]));
                models.push_back(model);
                released(ResourceType::MODEL, "models/shared.gltf");
                released(ResourceType::TEXTURE, PNG);
                ASSERT_TRUE(model->bounds());
                EXPECT_EQ(model->bounds()->min, models[0]->bounds()->min);
                EXPECT_EQ(model->bounds()->max, models[0]->bounds()->max);
                EXPECT_EQ(model->rootNode(), models[0]->rootNode());
                ASSERT_EQ(model->nodes().size(), 2u);
                EXPECT_EQ(model->nodes()[0].parts, models[0]->nodes()[0].parts);
                EXPECT_EQ(model->nodes()[1].parts, (std::vector<uint32_t>{0}));
                EXPECT_EQ(model->meshes().size(), 1u);
                EXPECT_EQ(model->textures().size(), 2u);
                EXPECT_EQ(model->materials().size(), 4u);
                ASSERT_EQ(model->draws().size(), 5u);
                for (size_t i = 0; i < model->parts().size(); ++i)
                {
                    EXPECT_EQ(model->parts()[i].geometry, 0u);
                    EXPECT_EQ(model->parts()[i].material, i);
                    EXPECT_EQ(model->draws()[i].model, models[0]->draws()[i].model);
                }
                for (bool minify : {false, true})
                {
                    SCOPED_TRACE(minify);
                    const auto extent = minify ? FrameExtent{1, 1} : FrameExtent{256, 256};
                    const auto transform = glm::translate(glm::mat4(1), glm::vec3(minify ? -0.4f : 0, 0, 0.5f));
                    const std::array draws{DrawItem{model->meshes()[0], model->materials()[0], transform}};
                    const FrameInput input{extent, {glm::mat4(1), glm::mat4(1)}, draws};
                    std::vector<uint8_t> pixels;
                    Detail::RendererReviewAccess::withFrame(
                        renderer, input,
                        [&](const auto& device, const auto& pipeline, auto frameSet, auto nativeDraws) {
                            pixels =
                                TextureGpuReview::captureDraws(device, pipeline, frameSet, nativeDraws,
                                                               {0, 0, float(extent.width), float(extent.height), 0, 1});
                        });
                    ASSERT_EQ(pixels.size(), 384u * 256 * 4);
                    const size_t offset = minify ? 0 : (128 * 384 + 128) * 4;
                    const int expected = minify ? (policy == 2 ? 188 : 255) : (policy == 0 ? 255 : 188);
                    for (int channel = 0; channel < 3; ++channel)
                        EXPECT_NEAR(pixels[offset + channel], expected, 2);
                    EXPECT_EQ(pixels[offset + 3], 255);
                    WindowCapture::save({384, 256, pixels}, "review/sampling-policy-" + std::to_string(policy) +
                                                                (minify ? "-min.ppm" : "-mag.ppm"));
                }
            }
            EXPECT_FALSE(ResourceRegistry::instance().lookup(ResourceType::TEXTURE, "textures/unused.png").valid());
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 3u);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 6u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 12u);
            EXPECT_EQ(occurrences(messages_.str(), "2x2, format 43, 16 bytes, 2 mips"), 2u);
            EXPECT_EQ(occurrences(messages_.str(), "2x2, format 43, 16 bytes, 1 mips"), 1u);
            EXPECT_EQ(occurrences(messages_.str(), "1x1, format 43, 4 bytes, 1 mips"), 3u);
            EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline:"), 1u);
        }
        for (const auto& model : models)
            EXPECT_FALSE(model->materials()[0].valid());
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
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 0u);
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
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 0u);
            released(ResourceType::MODEL, GLTF);
            released(ResourceType::TEXTURE, PNG);
            shaders(renderer);
            model = Model::prepareModel(renderer, GLTF);
            released(ResourceType::MODEL, GLTF);
            released(ResourceType::TEXTURE, PNG);
            for (int frame = 0; frame < 3; ++frame)
                draw(*native, renderer, *model);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 4u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 2u);
            EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline:"), 1u);
        }
        EXPECT_FALSE(model->meshes()[0].valid());
        EXPECT_FALSE(model->materials()[0].valid());
        EXPECT_FALSE(model->textures()[0].valid());
        EXPECT_EQ(model->source(), Model::ModelReference(GLTF));
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, UnusedPartsAndDependenciesCreateNoGpuResources)
    {
        ResourceManager::instance().registerLoader(ResourceType::MODEL, std::make_unique<SharedModelLoader>(true));
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            Model::PrepareOptions invalid;
            invalid.mipMode = static_cast<TextureMipMode>(99);
            EXPECT_THROW(Model::prepareModel(renderer, "models/empty.gltf", invalid), Error::Exception);
            invalid = {};
            invalid.sampler.magFilter = static_cast<TextureFilter>(99);
            EXPECT_THROW(Model::prepareModel(renderer, "models/empty.gltf", invalid), Error::Exception);
            EXPECT_FALSE(ResourceRegistry::instance().lookup(ResourceType::MODEL, "models/empty.gltf").valid());
            auto model = Model::prepareModel(renderer, "models/empty.gltf");
            EXPECT_EQ(model->source(), Model::ModelReference("models/empty.gltf"));
            EXPECT_TRUE(model->meshes().empty());
            EXPECT_TRUE(model->materials().empty());
            EXPECT_TRUE(model->textures().empty());
            EXPECT_TRUE(model->parts().empty());
            EXPECT_TRUE(model->draws().empty());
            EXPECT_FALSE(model->bounds());
            EXPECT_EQ(model->nodes().size(), 2u);
            Scene::Scene scene;
            scene.createEntity(model);
            const auto captured = Scene::captureScene(scene, {0, 0, 0, 1});
            ASSERT_EQ(captured.entities.size(), 1u);
            ASSERT_TRUE(captured.entities[0].model);
            EXPECT_EQ(captured.entities[0].model->path(), "models/empty.gltf");
            EXPECT_TRUE(Scene::extractSceneFrame(scene, {400, 400})->draws.empty());
            std::weak_ptr<const Model::PreparedModel> weak = model;
            scene.clear();
            model.reset();
            EXPECT_TRUE(weak.expired());
            EXPECT_EQ(captured.entities[0].model->path(), "models/empty.gltf");
            released(ResourceType::MODEL, "models/empty.gltf");
            EXPECT_FALSE(ResourceRegistry::instance().lookup(ResourceType::TEXTURE, PNG).valid());
            EXPECT_FALSE(ResourceRegistry::instance().lookup(ResourceType::TEXTURE, "textures/unused.png").valid());
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded"), 0u);
        }
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, SceneRebuildReusesSessionModelsAndPreservesFailures)
    {
        using Demo::ModelCache;
        auto native = window();
        std::weak_ptr<const Model::PreparedModel> firstSession;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            ModelCache cache(renderer);
            const Model::ModelReference gltf("models/minimal_gltf/model.gltf");
            const Model::ModelReference obj("models/minimal_obj/model.obj");
            const auto first = cache.resolve(gltf);
            firstSession = first;
            EXPECT_EQ(first, cache.resolve(Model::ModelReference("MODELS/minimal_gltf/../minimal_gltf/MODEL.gltf")));
            const auto second = cache.resolve(obj);
            Scene::SceneDescription description;
            description.entities = {{gltf, Scene::Transform({1, 2, 3}, {1, 0, 0, 0}, {-2, 0, 3}), false},
                                    {obj, Scene::Transform({-1, 0, 0}), true},
                                    {gltf, Scene::Transform({4, 0, 0}), true},
                                    {std::nullopt, Scene::Transform{}, true}};
            auto resolver = [&](const Model::ModelReference& ref) { return cache.resolve(ref); };
            const auto uploads = occurrences(messages_.str(), "Uploaded mesh:");
            auto built = Scene::rebuildScene(description, resolver);
            ASSERT_EQ(built.scene->entities().size(), 4u);
            EXPECT_EQ(built.scene->entities()[0].model, first);
            EXPECT_EQ(built.scene->entities()[1].model, second);
            EXPECT_EQ(built.scene->entities()[2].model, first);
            EXPECT_EQ(built.scene->entities()[3].model, nullptr);
            EXPECT_FALSE(built.scene->entities()[0].visible);
            EXPECT_EQ(built.scene->entities()[0].transform.matrix(), description.entities[0].transform.matrix());
            const auto captured = Scene::captureScene(*built.scene, built.clearColor);
            EXPECT_EQ(captured.entities[0].model, description.entities[0].model);
            EXPECT_EQ(captured.entities[1].model, description.entities[1].model);
            fs::create_directories(sandbox_ / "outside");
            const auto scenePath =
                Utils::Utf8::toUtf8String(sandbox_ / "outside" / Utils::Utf8::toFsPath("场景😀.json"));
            Scene::writeScene(*fileIO_, scenePath, captured);
            const auto loaded = Scene::readScene(*fileIO_, scenePath);
            EXPECT_EQ(Scene::rebuildScene(loaded, resolver).scene->entities()[0].model, first);
            auto again = Scene::rebuildScene(captured, resolver);
            EXPECT_EQ(again.scene->entities()[0].model, first);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), uploads);
            const auto oldId = built.scene->entities()[0].id;
            EXPECT_EQ(occurrences(messages_.str(), "Prepared model '"), 2u);
            auto missing = description;
            missing.entities[1].model = Model::ModelReference("models/missing.gltf");
            EXPECT_THROW(Scene::rebuildScene(missing, resolver), Error::Exception);
            EXPECT_NE(built.scene->find(oldId), nullptr);
            EXPECT_EQ(Scene::rebuildScene(description, resolver).scene->entities()[0].model, first);

            const auto texturePath = fs::path("assets") / PNG;
            fs::remove(texturePath);
            Model::PrepareOptions missingTextureOptions;
            missingTextureOptions.sampler.mipFilter = MipFilter::Linear;
            auto missingTexture = description;
            missingTexture.entities[0].model = Model::ModelReference(gltf.path(), missingTextureOptions);
            EXPECT_THROW(Scene::rebuildScene(missingTexture, resolver), Error::Exception);
            EXPECT_NE(built.scene->find(oldId), nullptr);
            fs::copy_file(Utils::Utf8::toFsPath(RUNTIME_TEST_ASSET_DIR) / PNG, texturePath);
            EXPECT_NE(cache.resolve(*missingTexture.entities[0].model), first);

            Model::PrepareOptions option;
            option.mipMode = TextureMipMode::FullChain;
            EXPECT_NE(cache.resolve(Model::ModelReference(gltf.path(), option)), first);
            option = {};
            option.sampler.minFilter = TextureFilter::Linear;
            EXPECT_NE(cache.resolve(Model::ModelReference(gltf.path(), option)), first);
            option = {};
            option.sampler.magFilter = TextureFilter::Linear;
            EXPECT_NE(cache.resolve(Model::ModelReference(gltf.path(), option)), first);
            option = {};
            option.sampler.mipFilter = MipFilter::Nearest;
            EXPECT_NE(cache.resolve(Model::ModelReference(gltf.path(), option)), first);
            option = {};
            option.shading = ShadingMode::Lambert;
            EXPECT_NE(cache.resolve(Model::ModelReference(gltf.path(), option)), first);
            released(ResourceType::MODEL, gltf.path());
            EXPECT_TRUE(ResourceManager::instance().hasLoader(ResourceType::MODEL));
        }
        EXPECT_TRUE(firstSession.expired());
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            ModelCache nextSession(renderer);
            const auto next = nextSession.resolve(Model::ModelReference(GLTF));
            EXPECT_TRUE(next->meshes()[0].valid());
            EXPECT_TRUE(firstSession.expired());
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
        explicit TransformModelLoader(std::optional<glm::mat4> leafTransform = std::nullopt)
            : leafTransform_(leafTransform)
        {}

        std::unique_ptr<ResourceObject> load(const ResourceId& id, std::string_view) override
        {
            MeshData mesh{{{{0, 0, 0}, {0, 0}}, {{1, 0, 0}, {1, 0}}, {{0, 1, 0}, {0, 1}}}, {0, 1, 2}};
            mesh.vertices[0].normal = {1, 1, 0};
            mesh.vertices[1].normal = {0, 1, 1};
            mesh.vertices[2].normal = {1, 0, 1};
            const auto root = glm::translate(glm::mat4(1), glm::vec3(10, 0, 0));
            const auto parent = glm::rotate(glm::mat4(1), glm::radians(90.0f), glm::vec3(0, 0, 1));
            const auto leaf = leafTransform_.value_or(glm::translate(glm::mat4(1), glm::vec3(2, 0, 0)) *
                                                      glm::scale(glm::mat4(1), glm::vec3(2, 3, 1)));
            auto unused = mesh;
            for (auto& vertex : unused.vertices)
                vertex.position = {1000, 2000, 3000};
            return std::make_unique<ModelResource>(
                id, std::vector<MeshData>{unused, mesh},
                std::vector<ModelMaterial>{{}, {std::nullopt, {0.5f, 1, 0.5f}}, {std::nullopt, {1, 0, 0}}},
                std::vector<ModelPart>{{1, 0}, {1, 1}, {1, 2}, {0, 0}},
                std::vector<ModelNode>{{"parent", parent, {1}, {0}},
                                       {"leaf", leaf, {}, {1, 0}},
                                       {"root", root, {0, 3}, {2}},
                                       {"sibling", glm::translate(glm::mat4(1), glm::vec3(-1, 0, 0)), {}, {2}}},
                2);
        }

    private:
        std::optional<glm::mat4> leafTransform_;
    };

    TEST_F(ModelPreparationTest, ShadowDepthUsesSourceTransformsAndKeepsInFlightSlotsAcrossToggles)
    {
        ResourceManager::instance().registerLoader(ResourceType::MODEL, std::make_unique<TransformModelLoader>());
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            shadowShader(renderer);
            auto model = Model::prepareModel(renderer, "models/shadow-source.gltf");
            const Scene::Transform pose({1, 2, 2}, {1, 0, 0, 1}, {2, -3, 0});
            const std::array instances{Model::ModelInstance{model, pose.matrix()}};
            auto casters = Model::makeModelDraws(instances);
            for (auto& draw : casters)
                draw.material = {};
            ShadowSettings settings;
            settings.enabled = true;
            settings.center = {2, 68.0f / 3, 0};
            settings.halfExtent = {3, 3};
            settings.nearPlane = 1;
            settings.farPlane = 9;
            for (int i = 0; i < 10; ++i)
            {
                native->pollEvents();
                ASSERT_TRUE(renderer.beginUiFrame());
                const FrameExtent extent{i < 4 ? 192u : 320u, 128};
                auto output = renderer.prepareSceneTarget(extent);
                settings.enabled = i != 4 && i != 7;
                FrameInput input{extent, {glm::mat4(1), glm::mat4(1)},          {},
                                 {},     makeShadowFrame(settings, {0, 0, -1}), casters};
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(output, input, {0, 0, 0, 1});
                if (i == 0)
                    Detail::RendererReviewAccess::withShadowOutput(
                        renderer, output, [&](const auto& device, const auto& image) {
                            // 源根平移先于实体旋转和缩放；已知三角形重心应在深度图中心且深度为 0.25。
                            EXPECT_NEAR(readShadowDepth(device, image), 0.25f, 0.00003f);
                        });
            }
            // 后续帧不额外等待回读，关闭时由帧槽持有和 Fence 保证深度资源仍然有效。
            casters.clear();
            model.reset();
        }
        EXPECT_EQ(occurrences(messages_.str(), "Created shadow target:"), 2u);
        EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline: color format 0,"), 1u);
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, UnavailablePartBoundsInvalidateWholeModelWithoutDiscardingDraws)
    {
        glm::mat4 projective(1.0f);
        projective[0][3] = 0.5f;
        glm::mat4 overflow(1.0f);
        overflow[0][0] = std::numeric_limits<float>::max();
        overflow[3][0] = std::numeric_limits<float>::max();
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            for (const auto& matrix : {projective, overflow})
            {
                ResourceManager::instance().registerLoader(ResourceType::MODEL,
                                                           std::make_unique<TransformModelLoader>(matrix));
                const auto model = Model::prepareModel(renderer, "models/unavailable-bounds.gltf");
                released(ResourceType::MODEL, "models/unavailable-bounds.gltf");
                EXPECT_FALSE(model->bounds());
                ASSERT_EQ(model->draws().size(), 5u);
                Scene::Scene scene;
                scene.createEntity(model, Scene::Transform({1000, 0, -10}));
                EXPECT_EQ(Scene::extractSceneFrame(scene, {400, 400})->draws.size(), 5u);
                EXPECT_EQ(model->meshes().size(), 1u);
                EXPECT_EQ(model->materials().size(), 3u);
                for (const auto& item : model->draws())
                {
                    EXPECT_TRUE(item.mesh.valid());
                    EXPECT_TRUE(item.material.valid());
                }
                // 有效的根节点和父节点绘制项位于无效叶节点之前，之后还有一个有效兄弟节点。
                EXPECT_EQ(model->draws().back().model, glm::translate(glm::mat4(1), glm::vec3(9, 0, 0)));
            }
        }
        cleanValidation();
    }

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
                                          std::span<const Model::ModelInstance> instances)
        {
            window.pollEvents();
            const auto extent = renderer.beginUiFrame();
            if (!extent)
                return std::nullopt;
            auto draws = Model::makeModelDraws(instances);
            renderer.imgui().endFrame();
            renderer.drawFrame({*extent, camera(*extent), draws}, {0.2f, 0.24f, 0.28f, 1});
            // GPU 必然完成前，修改并销毁已提交的 CPU 存储。
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
            source_.reset();
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
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
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

    TEST_F(SceneFrameTest, ShadowCastersIgnoreCameraCullingAndCopiedMovedFramesOwnBothLists)
    {
        ResourceManager::instance().registerLoader(ResourceType::MODEL, std::make_unique<TransformModelLoader>());
        auto native = window();
        Scene::SceneFrame retained;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            auto model = Model::prepareModel(renderer, "models/shadow-transforms.gltf");
            ASSERT_EQ(model->draws().size(), 5u);
            ASSERT_NE(model->nodes()[model->rootNode()].localTransform, glm::mat4(1));
            Scene::Scene scene;
            scene.setCamera(Scene::Camera({0, 0, 80}, {1, 0, 0, 0}, {glm::radians(90.0f), 0.1f, 200}));
            const std::array poses{Scene::Transform({1, 2, 0}, {1, 0, 0, 1}, {2, 3, 1}),
                                   Scene::Transform({10000, 0, 0}),
                                   Scene::Transform({-2, 1, 0}, {1, 0, 0, 0}, {-0.5f, 2, 0})};
            const auto firstId = scene.createEntity(model, poses[0]);
            scene.createEntity({}, Scene::Transform{});
            scene.createEntity(model, Scene::Transform{}, false);
            scene.createEntity(model, poses[1]);
            scene.createEntity(model, poses[2]);
            Render::ShadowSettings settings;
            settings.enabled = true;
            auto first = Scene::extractSceneFrame(scene, {800, 400}, true, settings);
            ASSERT_TRUE(first && first->shadow);
            ASSERT_EQ(first->draws.size(), 10u);
            ASSERT_EQ(first->shadowDraws.size(), 15u);
            for (size_t entity = 0; entity < poses.size(); ++entity)
                for (size_t part = 0; part < model->draws().size(); ++part)
                    EXPECT_EQ(first->shadowDraws[entity * 5 + part].model,
                              poses[entity].matrix() * model->draws()[part].model);
            // 夹具的根及子节点不与实体旋转、缩放交换，已知点可发现重复应用源根的问题。
            point(first->shadowDraws[0].model, -2, 24);
            const auto uncull = Scene::extractSceneFrame(scene, {800, 400}, false, settings);
            ASSERT_EQ(uncull->draws.size(), 15u);
            for (size_t i = 0; i < first->shadowDraws.size(); ++i)
                EXPECT_EQ(uncull->draws[i].model, first->shadowDraws[i].model);
            const auto disabled = Scene::extractSceneFrame(scene, {800, 400});
            EXPECT_FALSE(disabled->shadow);
            EXPECT_TRUE(disabled->shadowDraws.empty());
            EXPECT_EQ(disabled->draws.size(), first->draws.size());

            auto copied = *first;
            EXPECT_NE(copied.draws.data(), first->draws.data());
            EXPECT_NE(copied.shadowDraws.data(), first->shadowDraws.data());
            auto moved = std::move(copied);
            retained = moved;
            Scene::SceneFrame moveAssigned;
            moveAssigned = std::move(moved);
            for (const auto* frame : {&retained, &moveAssigned})
            {
                EXPECT_EQ(frame->input().draws.data(), frame->draws.data());
                EXPECT_EQ(frame->input().shadowDraws.data(), frame->shadowDraws.data());
                EXPECT_NE(frame->shadowDraws.data(), first->shadowDraws.data());
            }
            const auto previousMatrix = first->shadow->viewProjection;
            scene.setTransform(firstId, Scene::Transform({50, 0, 0}));
            scene.setLighting({{0, 1, 0}, {1, 1, 1}, 1, 0.1f});
            const auto changed = Scene::extractSceneFrame(scene, {400, 400}, true, settings);
            EXPECT_NE(changed->shadowDraws[0].model, first->shadowDraws[0].model);
            EXPECT_NE(changed->shadow->viewProjection, previousMatrix);
            EXPECT_EQ(retained.shadow->viewProjection, previousMatrix);
            scene.clear();
            model.reset();
            const auto empty = Scene::extractSceneFrame(scene, {400, 400}, true, settings);
            EXPECT_TRUE(empty->shadow.has_value());
            EXPECT_TRUE(empty->draws.empty());
            EXPECT_TRUE(empty->shadowDraws.empty());
            first.reset();
            EXPECT_TRUE(retained.shadowDraws[0].mesh.valid());
            point(retained.shadowDraws[0].model, -2, 24);
        }
        EXPECT_FALSE(retained.shadowDraws[0].mesh.valid());
        cleanValidation();
    }

    TEST_F(SceneFrameTest, CullingKeepsEntityOrderAndUsesCurrentWorldTransformBeforeExpansion)
    {
        ResourceManager::instance().registerLoader(ResourceType::MODEL, std::make_unique<TransformModelLoader>());
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            const auto model = Model::prepareModel(renderer, "models/culling.gltf");
            ASSERT_EQ(model->draws().size(), 5u);
            released(ResourceType::MODEL, "models/culling.gltf");
            ResourceManager::instance().reset();
            source_.reset();
            fileIO_.reset();
            const auto resourceLog = resourceMessages_.str();
            const auto renderLog = messages_.str();
            Scene::Scene scene;
            scene.setCamera(Scene::Camera({}, {1, 0, 0, 0}, {glm::radians(90.0f), 1, 100}));
            const Scene::Transform a({-9, -2, -10});
            const Scene::Transform outside({1000, 0, -10});
            const Scene::Transform b({-9, -2, -12});
            const auto first = scene.createEntity(model, a);
            const auto middle = scene.createEntity(model, outside);
            const auto last = scene.createEntity(model, b);
            scene.createEntity(model, a, false);
            scene.createEntity();
            const auto snapshot = Scene::extractSceneFrame(scene, {400, 400});
            ASSERT_TRUE(snapshot);
            ASSERT_EQ(snapshot->draws.size(), 10u);
            for (size_t i = 0; i < model->draws().size(); ++i)
            {
                EXPECT_EQ(snapshot->draws[i].model, a.matrix() * model->draws()[i].model);
                EXPECT_EQ(snapshot->draws[i + 5].model, b.matrix() * model->draws()[i].model);
            }
            const std::array all{Model::ModelInstance{model, a.matrix()}, Model::ModelInstance{model, outside.matrix()},
                                 Model::ModelInstance{model, b.matrix()}};
            const auto direct = Model::makeModelDraws(all);
            const auto disabled = Scene::extractSceneFrame(scene, {400, 400}, false);
            ASSERT_TRUE(disabled);
            ASSERT_EQ(disabled->draws.size(), direct.size());
            EXPECT_EQ(direct.size(), 15u);
            for (size_t i = 0; i < direct.size(); ++i)
                EXPECT_EQ(disabled->draws[i].model, direct[i].model);
            ASSERT_TRUE(scene.setTransform(middle, a));
            EXPECT_EQ(Scene::extractSceneFrame(scene, {400, 400})->draws.size(), 15u);
            for (const auto id : {first, middle, last})
                ASSERT_TRUE(scene.setTransform(id, outside));
            const auto empty = Scene::extractSceneFrame(scene, {400, 400});
            ASSERT_TRUE(empty);
            EXPECT_TRUE(empty->input().draws.empty());
            EXPECT_TRUE(scene.find(first)->visible);
            EXPECT_EQ(scene.entities().size(), 5u);
            EXPECT_EQ(snapshot->draws.size(), 10u);

            scene.destroyEntity(middle);
            scene.destroyEntity(last);
            struct Example
            {
                Scene::Transform transform;
                size_t draws;
            };
            const std::array<Example, 7> examples{{
                {a, 5},
                {outside, 0},
                {Scene::Transform({2, -9, -10}, {1, 0, 0, 1}), 5},
                {Scene::Transform({18, -6, -10}, {1, 0, 0, 0}, {-2, 3, 1}), 5},
                {Scene::Transform({0, 0, -10}, {1, 0, 0, 0}, glm::vec3(0)), 5},
                {Scene::Transform({20, 0, -10}, {1, 0, 0, 0}, glm::vec3(0)), 0},
                {Scene::Transform({0, 0, -10}, {1, 0, 0, 0}, {std::numeric_limits<float>::max(), 1, 1}), 5},
            }};
            for (size_t i = 0; i < examples.size(); ++i)
            {
                SCOPED_TRACE(i);
                ASSERT_TRUE(scene.setTransform(first, examples[i].transform));
                const auto frame = Scene::extractSceneFrame(scene, {400, 400});
                ASSERT_TRUE(frame);
                EXPECT_EQ(frame->draws.size(), examples[i].draws);
            }
            EXPECT_FALSE(Scene::extractSceneFrame(scene, {0, 400}));
            EXPECT_EQ(resourceMessages_.str(), resourceLog);
            EXPECT_EQ(messages_.str(), renderLog);
        }
        cleanValidation();
    }

    TEST_F(SceneFrameTest, CullingReflectsCameraExtentVisibilityModelAndSceneChanges)
    {
        auto& manager = ResourceManager::instance();
        manager.registerLoader(ResourceType::MODEL, std::make_unique<TransformModelLoader>());
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            const auto model = Model::prepareModel(renderer, "models/transforms.gltf");
            manager.registerLoader(ResourceType::MODEL, std::make_unique<SharedModelLoader>());
            const auto centered = Model::prepareModel(renderer, "models/centered.gltf");
            released(ResourceType::MODEL, "models/transforms.gltf");
            released(ResourceType::MODEL, "models/centered.gltf");
            released(ResourceType::TEXTURE, PNG);
            manager.reset();
            source_.reset();
            fileIO_.reset();
            const auto resourceLog = resourceMessages_.str();
            const auto renderLog = messages_.str();
            Scene::Scene scene;
            const Scene::Camera wide({}, {1, 0, 0, 0}, {glm::radians(90.0f), 1, 100});
            scene.setCamera(wide);
            const auto id = scene.createEntity(model, Scene::Transform({-2, -2, -10}));
            const auto count = [&](FrameExtent extent = {400, 400}) {
                const auto frame = Scene::extractSceneFrame(scene, extent);
                EXPECT_TRUE(frame);
                if (!frame)
                    return size_t{0};
                const auto expected = scene.camera().matrices(extent);
                EXPECT_EQ(frame->camera.view, expected.view);
                EXPECT_EQ(frame->camera.projection, expected.projection);
                return frame->draws.size();
            };
            EXPECT_EQ(count(), 5u);
            auto narrow = wide;
            narrow.setPerspective({glm::radians(45.0f), 1, 100});
            scene.setCamera(narrow);
            EXPECT_EQ(count(), 0u);
            EXPECT_EQ(count({800, 400}), 5u);
            scene.setCamera(wide);
            EXPECT_EQ(count({100, 400}), 0u);
            auto moved = wide;
            moved.setPosition({30, 0, 0});
            scene.setCamera(moved);
            EXPECT_EQ(count(), 0u);
            moved.setPosition({0, 0, 0});
            moved.setRotation({0, 0, 1, 0});
            scene.setCamera(moved);
            EXPECT_EQ(count(), 0u);
            moved.setPosition({0, 0, -20});
            scene.setCamera(moved);
            EXPECT_EQ(count(), 5u);
            scene.setCamera(wide);
            ASSERT_TRUE(scene.setVisible(id, false));
            EXPECT_EQ(count(), 0u);
            ASSERT_TRUE(scene.setVisible(id, true));
            EXPECT_EQ(count(), 5u);
            ASSERT_TRUE(scene.setTransform(id, Scene::Transform({0, 0, -5})));
            EXPECT_EQ(count(), 0u);
            ASSERT_TRUE(scene.setModel(id, centered));
            EXPECT_EQ(count(), 5u);
            ASSERT_TRUE(scene.setModel(id, {}));
            EXPECT_EQ(count(), 0u);

            Scene::Scene replacement;
            replacement.setCamera(wide);
            replacement.createEntity(model, Scene::Transform({-9, -2, -10}));
            EXPECT_EQ(Scene::extractSceneFrame(replacement, {400, 400})->draws.size(), 5u);
            EXPECT_EQ(count(), 0u);
            EXPECT_EQ(resourceMessages_.str(), resourceLog);
            EXPECT_EQ(messages_.str(), renderLog);
        }
        cleanValidation();
    }

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
            source_.reset();
            fileIO_.reset();
            const auto resourceLog = resourceMessages_.str();
            Scene::Scene scene;
            scene.setCamera(Scene::Camera({0, 0, 80}, {1, 0, 0, 0}, {glm::radians(90.0f), 0.1f, 200}));
            const Scene::Transform a({1, 2, 0}, {1, 0, 0, 1}, {2, 3, 1});
            const Scene::Transform b({-2, 1, 0}, {1, 0, 0, 0}, {0.5f, 2, 1});
            const auto firstId = scene.createEntity(model, a);
            scene.createEntity();
            scene.createEntity(model, Scene::Transform{}, false);
            const auto secondId = scene.createEntity(model, b);
            auto first = Scene::extractSceneFrame(scene, {800, 400});
            ASSERT_TRUE(first);
            ASSERT_EQ(first->draws.size(), 10u);
            const auto description = Scene::captureScene(scene, {0, 0, 0, 1});
            auto rebuilt = Scene::rebuildScene(description, [&](const Model::ModelReference&) { return model; });
            const auto rebuiltFrame = Scene::extractSceneFrame(*rebuilt.scene, {800, 400});
            ASSERT_TRUE(rebuiltFrame);
            ASSERT_EQ(rebuiltFrame->draws.size(), first->draws.size());
            for (size_t index = 0; index < first->draws.size(); ++index)
                EXPECT_EQ(rebuiltFrame->draws[index].model, first->draws[index].model);
            rebuilt.scene.reset();
            const std::array<std::array<float, 2>, 5> cached = {{{11, 1}, {9, 1}, {7, 4}, {7, 4}, {10, 1}}};
            for (size_t index = 0; index < cached.size(); ++index)
            {
                point(first->draws[index].model, 1 - 3 * cached[index][1], 2 + 2 * cached[index][0]);
                point(first->draws[index + 5].model, -2 + 0.5f * cached[index][0], 1 + 2 * cached[index][1]);
            }
            Detail::RendererReviewAccess::withFrame(
                renderer, first->input(), [&](const auto&, const auto&, auto, auto nativeDraws) {
                    ASSERT_EQ(nativeDraws.size(), first->draws.size());
                    for (size_t index = 0; index < nativeDraws.size(); ++index)
                    {
                        const auto& object = nativeDraws[index].object;
                        const auto reference = glm::transpose(glm::inverse(glm::dmat3(first->draws[index].model)));
                        for (int column = 0; column < 4; ++column)
                            EXPECT_EQ(object.model[column], first->draws[index].model[column]);
                        for (int column = 0; column < 3; ++column)
                        {
                            EXPECT_EQ(object.normalColumns[column].w, 0);
                            for (int row = 0; row < 3; ++row)
                                EXPECT_NEAR(object.normalColumns[column][row], reference[column][row], 0.00001);
                        }
                    }
                });
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
            scene.setCamera(Scene::Camera({0, 0, 100}, {1, 0, 0, 0}, {glm::radians(90.0f), 0.1f, 200}));
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
            scene.createEntity(model, a);  // 新实体排在仍然存在的第二个实体之后。
            const auto reordered = Scene::extractSceneFrame(scene, {800, 400});
            ASSERT_TRUE(reordered);
            ASSERT_EQ(reordered->draws.size(), 10u);
            point(reordered->draws[0].model, 3.5f, 3);
            point(reordered->draws[5].model, -2, 24);
            ASSERT_TRUE(scene.setModel(secondId, {}));
            EXPECT_EQ(Scene::extractSceneFrame(scene, {800, 400})->draws.size(), 5u);
            scene.clear();
            model.reset();
            EXPECT_TRUE(weak.expired());  // 帧持有值和 GPU 引用，不持有 PreparedModel。
            EXPECT_TRUE(Scene::extractSceneFrame(scene, {800, 400})->draws.empty());
            first.reset();
            point(retained.draws[0].model, -2, 24);
            EXPECT_EQ(retained.input().draws.data(), retained.draws.data());
            EXPECT_TRUE(retained.draws[0].mesh.valid());
            EXPECT_EQ(resourceMessages_.str(), resourceLog);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 1u);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 1u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 3u);
            EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline:"), 1u);
        }
        EXPECT_FALSE(retained.draws[0].mesh.valid());
        EXPECT_FALSE(retained.draws[0].material.valid());
        cleanValidation();
    }

    TEST_F(SceneFrameTest, ImportedSnapshotSubmitsAfterSceneClear)
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
            scene.createEntity(gltf, placement(-0.65f, 0.45f, 0.25f));
            scene.createEntity(gltf, placement(0.65f, 0.45f, 0.25f));
            scene.createEntity(obj, placement(-0.65f, -0.55f, 0.35f));
            scene.createEntity(obj, placement(0.65f, -0.55f, 0.35f));
            scene.createEntity();
            released(ResourceType::MODEL, GLTF);
            released(ResourceType::MODEL, OBJ);
            released(ResourceType::TEXTURE, PNG);
            ResourceManager::instance().reset();
            source_.reset();
            fileIO_.reset();

            native->pollEvents();
            const auto extent = renderer.beginUiFrame();
            ASSERT_TRUE(extent);
            auto retained = Scene::extractSceneFrame(scene, *extent);
            ASSERT_TRUE(retained);
            ASSERT_EQ(retained->draws.size(), 12u);
            scene.clear();
            EXPECT_TRUE(scene.entities().empty());
            // 场景已清空，仍通过快照自有的绘制列表提交原来的模型。
            renderer.imgui().endFrame();
            renderer.drawFrame(retained->input(), {0.2f, 0.24f, 0.28f, 1});
            retained.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            const auto image = WindowCapture::capture(*native);
            color(image, -0.510897f, 0.567851f, {188, 255, 188});
            color(image, 0.789103f, 0.567851f, {188, 255, 188});
            color(image, -0.186806f, 0.261438f, {188, 255, 188});
            color(image, -0.44f, -0.608333f, {188, 255, 188});
            color(image, 0.86f, -0.608333f, {188, 255, 188});
            WindowCapture::save(image, "review/scene-frame-retained-after-clear.ppm");
        }
        cleanValidation();
    }

    TEST_F(ModelInstanceTest, LambertInterpolatedNormalsMatchFinalRootNodeEntityInverseTranspose)
    {
        ResourceManager::instance().registerLoader(ResourceType::MODEL, std::make_unique<TransformModelLoader>());
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            Model::PrepareOptions options;
            options.shading = ShadingMode::Lambert;
            const auto model = Model::prepareModel(renderer, "models/normal-transforms.gltf", options);
            released(ResourceType::MODEL, "models/normal-transforms.gltf");
            EXPECT_EQ(model->rootNode(), 2u);
            Scene::Scene scene;
            const auto entity = scene.createEntity(model);
            const Lighting lighting{{-0.3f, -0.6f, -1}, {0.4f, 0.8f, 0.6f}, 0.8f, 0.12f};
            scene.setLighting(lighting);
            const auto root = glm::translate(glm::mat4(1), glm::vec3(10, 0, 0));
            const auto parent = glm::rotate(glm::mat4(1), glm::radians(90.0f), glm::vec3(0, 0, 1));
            const auto leaf =
                glm::translate(glm::mat4(1), glm::vec3(2, 0, 0)) * glm::scale(glm::mat4(1), glm::vec3(2, 3, 1));
            const glm::quat rotation(glm::vec3(0.3f, 0.4f, 0.2f));
            const std::array<glm::vec3, 3> positions{glm::vec3(0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0)};
            const std::array<glm::vec3, 3> normals{glm::vec3(1, 1, 0), glm::vec3(0, 1, 1), glm::vec3(1, 0, 1)};
            for (const auto& scale : {glm::vec3(1), glm::vec3(2, 3, 0.5f), glm::vec3(-2, 3, 0.5f), glm::vec3(0.001f)})
            {
                SCOPED_TRACE(scale.x);
                const Scene::Transform transform({-0.5f, 0.3f, 0.1f}, rotation, scale);
                ASSERT_TRUE(scene.setTransform(entity, transform));
                const auto expectedModel = glm::translate(glm::mat4(1), transform.position()) *
                                           glm::mat4_cast(rotation) * glm::scale(glm::mat4(1), scale) * root * parent *
                                           leaf;
                auto snapshot = Scene::extractSceneFrame(scene, {384, 256}, false);
                ASSERT_TRUE(snapshot);
                ASSERT_EQ(snapshot->draws.size(), 5u);
                std::array draws{snapshot->draws[2]};
                for (int column = 0; column < 4; ++column)
                    for (int row = 0; row < 4; ++row)
                        EXPECT_NEAR(draws[0].model[column][row], expectedModel[column][row], 0.00001f);
                const auto center = glm::vec3(expectedModel * glm::vec4(1.0f / 3, 1.0f / 3, 0, 1));
                float radius = 0;
                for (const auto& position : positions)
                    radius = std::max(radius, glm::length(glm::vec3(expectedModel * glm::vec4(position, 1)) - center));
                radius *= 1.3f;
                auto projection = glm::orthoRH_ZO(-radius * 1.5f, radius * 1.5f, -radius, radius, 0.1f, 20.0f);
                projection[1][1] *= -1;
                const CameraMatrices camera{glm::lookAtRH(center + glm::vec3(0, 0, 10), center, glm::vec3(0, 1, 0)),
                                            projection};
                std::array<glm::vec2, 3> screen;
                std::array<glm::dvec3, 3> worldNormals;
                const auto normalMatrix = glm::transpose(glm::inverse(glm::dmat3(expectedModel)));
                for (size_t corner = 0; corner < 3; ++corner)
                {
                    const auto clip = camera.projection * camera.view * expectedModel * glm::vec4(positions[corner], 1);
                    screen[corner] = {(clip.x / clip.w + 1) * 192, (clip.y / clip.w + 1) * 128};
                    worldNormals[corner] = glm::normalize(normalMatrix * glm::dvec3(normals[corner]));
                }
                const auto cross = [](glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; };
                const glm::vec2 sample(192.5f, 128.5f);
                const float area = cross(screen[1] - screen[0], screen[2] - screen[0]);
                const float a = cross(screen[1] - sample, screen[2] - sample) / area;
                const float b = cross(screen[2] - sample, screen[0] - sample) / area;
                const float c = 1 - a - b;
                ASSERT_GT(std::min({a, b, c}), 0.05f);
                const auto interpolated = glm::normalize(double(a) * worldNormals[0] + double(b) * worldNormals[1] +
                                                         double(c) * worldNormals[2]);
                const double diffuse =
                    std::max(glm::dot(interpolated, -glm::normalize(glm::dvec3(lighting.direction))), 0.0);
                const auto expected =
                    glm::dvec3(0.5, 1, 0.5) *
                    (glm::dvec3(lighting.ambient) + glm::dvec3(lighting.color) * double(lighting.intensity) * diffuse);
                const auto pixels =
                    TextureGpuReview::captureFrame(renderer, {{384, 256}, camera, draws, snapshot->lighting});
                ASSERT_EQ(pixels.size(), 384u * 256 * 4);
                for (int channel = 0; channel < 3; ++channel)
                {
                    const double linear = std::clamp(expected[channel], 0.0, 1.0);
                    const double encoded =
                        linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
                    EXPECT_NEAR(pixels[(128 * 384 + 192) * 4 + channel], encoded * 255, 2);
                }
                EXPECT_EQ(pixels[(128 * 384 + 192) * 4 + 3], 255);
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
            ASSERT_TRUE(model->bounds());
            for (int axis = 0; axis < 3; ++axis)
            {
                EXPECT_NEAR(model->bounds()->min[axis], glm::vec3(7, 0, 0)[axis], 0.00001f);
                EXPECT_NEAR(model->bounds()->max[axis], glm::vec3(11, 4, 0)[axis], 0.00001f);
            }
            const std::array<std::array<float, 2>, 5> expected = {{{11, 1}, {9, 1}, {7, 4}, {7, 4}, {10, 1}}};
            for (size_t i = 0; i < expected.size(); ++i)
                point(model->draws()[i].model, expected[i][0], expected[i][1]);
            std::array instances = {Model::ModelInstance{model, glm::translate(glm::mat4(1), glm::vec3(0, 5, 0))},
                                    Model::ModelInstance{model, glm::translate(glm::mat4(1), glm::vec3(-2, 1, 0)) *
                                                                    glm::scale(glm::mat4(1), glm::vec3(0.5f, 2, 1))}};
            model.reset();
            EXPECT_EQ(instances[0].model.get(), instances[1].model.get());
            EXPECT_EQ(&instances[0].model->bounds(), &instances[1].model->bounds());
            const auto bounds = *instances[0].model->bounds();
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
            EXPECT_EQ(instances[0].model->bounds()->min, bounds.min);
            EXPECT_EQ(instances[1].model->bounds()->max, bounds.max);
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

    TEST_F(ModelInstanceTest, DemoModelsWithUnicodeDependenciesPreserveDirectionAndExactGpuColor)
    {
        // 保留真实的相对依赖链，同时将所有输入类型的文件名改为超出 ANSI 范围的字符。
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
        // 此测试采样桌面合成结果。呈现前先将窗口置顶，
        // 避免置顶状态恰好在 BitBlt 前才生效。
        ASSERT_TRUE(SetWindowPos(glfwGetWin32Window(static_cast<GLFWwindow*>(native->getNativeHandle())), HWND_TOPMOST,
                                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE));
        std::array<std::shared_ptr<const Model::PreparedModel>, 2> models;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            constexpr Model::PrepareOptions exactSampling{
                TextureMipMode::BaseLevel, {TextureFilter::Nearest, TextureFilter::Nearest, MipFilter::BaseLevel}};
            models = {Model::prepareModel(renderer, gltfPath, exactSampling),
                      Model::prepareModel(renderer, objPath, exactSampling)};
            ResourceManager::instance().reset();
            released(ResourceType::MODEL, gltfPath);
            released(ResourceType::MODEL, objPath);
            released(ResourceType::TEXTURE, pngPath);
            released(ResourceType::SHADER, "shaders/cube.vert.spv");
            released(ResourceType::SHADER, "shaders/cube.frag.spv");
            source_.reset();
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
                // 仅在色彩图视图中取消第一个源节点。完整的
                // 缓存层级仍会参与渲染；另有测试检查其源矩阵。
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
                    // 窗口创建后，等待桌面合成结果更新。
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
            }
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 4u);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 4u);
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
