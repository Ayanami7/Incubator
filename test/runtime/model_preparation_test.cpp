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

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/model/model_manager.h"
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
        for (const auto& variant : {Model::PrepareOptions{TextureMipMode::FullChain},
                                    Model::PrepareOptions{{}, {TextureFilter::Linear}},
                                    Model::PrepareOptions{{}, {TextureFilter::Nearest, TextureFilter::Linear}},
                                    Model::PrepareOptions{{}, {TextureFilter::Nearest, TextureFilter::Nearest, MipFilter::Nearest}},
                                    Model::PrepareOptions{{}, {TextureFilter::Nearest, TextureFilter::Nearest, MipFilter::Linear}},
                                    Model::PrepareOptions{{}, {}, ShadingMode::Lambert}})
            EXPECT_NE(base, Model::ModelReference("models/scene.gltf", variant));
        for (const auto& path : {std::string(""), std::string("../escape.gltf"), std::string("C:asset.gltf"),
                                 std::string("x\0y", 3), std::string("\xff", 1)})
            EXPECT_THROW((void)Model::ModelReference{path}, Error::Exception);
        for (const auto& bad : {Model::PrepareOptions{static_cast<TextureMipMode>(99)},
                                Model::PrepareOptions{{}, {static_cast<TextureFilter>(99)}},
                                Model::PrepareOptions{{}, {TextureFilter::Nearest, static_cast<TextureFilter>(99)}},
                                Model::PrepareOptions{{}, {TextureFilter::Nearest, TextureFilter::Nearest, static_cast<MipFilter>(99)}},
                                Model::PrepareOptions{{}, {}, static_cast<ShadingMode>(99)}})
            EXPECT_THROW((void)(Model::ModelReference{"models/scene.gltf", bad}), Error::Exception);
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
        Vulkan::VulkanMaterialBinding material(device, bindings.materialLayout(), image, sampler,
                                               bindings.normalFallback(), sampler,
                                               bindings.metallicRoughnessFallback(), sampler, {1, 1, 1});
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
            {
                const auto vertex = manager.hold<ShaderResource>(ResourceType::SHADER, "shaders/cube.vert.spv");
                const auto fragment = manager.hold<ShaderResource>(ResourceType::SHADER, "shaders/cube.frag.spv");
                ASSERT_TRUE(vertex);
                ASSERT_TRUE(fragment);
                renderer.prepareSceneResources(*vertex.get<ShaderResource>(), *fragment.get<ShaderResource>());
            }
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
            {
                const auto shader = manager.hold<ShaderResource>(ResourceType::SHADER, "shaders/shadow.vert.spv");
                ASSERT_TRUE(shader);
                renderer.prepareShadowResources(*shader.get<ShaderResource>());
            }
            released(ResourceType::SHADER, "shaders/shadow.vert.spv");
        }
        static std::vector<DrawItem> expandModel(const Model::PreparedModel& model, const glm::mat4& transform)
        {
            std::vector<DrawItem> result;
            result.reserve(model.draws().size());
            for (auto draw : model.draws())
            {
                draw.model = transform * draw.model;
                result.push_back(std::move(draw));
            }
            return result;
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
                EXPECT_NEAR(pixels[0], 31, 1);
                EXPECT_NEAR(pixels[1], 41, 1);
                EXPECT_NEAR(pixels[2], 56, 1);
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
                renderer.drawSceneTarget(output, frame->input(), {0.12f, 0.16f, 0.22f, 1}, [&](void* c) {
                    renderer.imgui().recordDrawData(renderer.imgui().drawData(), static_cast<VkCommandBuffer>(c));
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
                const auto& s = samples[i];
                auto frame = Scene::extractSceneFrame(
                    scene, {384, 256}, true, {true, s.center, s.halfExtent, s.nearPlane, s.farPlane, s.bias});
                ASSERT_TRUE(frame);
                frame->shadowDraws = {
                    {receiver->meshes()[0], {},
                     glm::translate(glm::mat4(1), {s.center.x, s.center.y, s.casterZ}) *
                         glm::scale(glm::mat4(1), {4, 4, 1})}};
                expectShadowPixel(*frame, shadowScenePixels(*native, renderer, *frame), {0, 0.75f, 0},
                                  s.occluded ? glm::vec3(lighting.ambient)
                                             : glm::vec3(lighting.ambient) + lighting.color * lighting.intensity);
            }
        }
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, ShadowDepthTargetsFollowSlotsAndSourceTransformsAcrossToggles)
    {
        ResourceManager::instance().registerLoader(ResourceType::MODEL, std::make_unique<TransformModelLoader>());
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            shadowShader(renderer);
            auto& manager = ResourceManager::instance();
            const auto vert = manager.hold<ShaderResource>(ResourceType::SHADER, "shaders/cube.vert.spv");
            const auto frag = manager.hold<ShaderResource>(ResourceType::SHADER, "shaders/cube.frag.spv");
            ASSERT_TRUE(vert && frag);
            const auto vertWords = vert.get<ShaderResource>()->words();
            const auto fragWords = frag.get<ShaderResource>()->words();

            const std::array<MeshVertex, 4> vertices{
                {{{-2, -2, 0}, {0, 0}}, {{2, -2, 0}, {1, 0}}, {{2, 2, 0}, {1, 1}}, {{-2, 2, 0}, {0, 1}}}};
            const auto mesh = renderer.createMesh(vertices, std::array<uint32_t, 6>{0, 1, 2, 0, 2, 3});
            const auto reversed = renderer.createMesh(vertices, std::array<uint32_t, 6>{2, 1, 0, 3, 2, 0});
            ShadowSettings settings{true, {0, 0, 0}, {4, 4}, 1, 9, 0.001f};
            const auto shadow = makeShadowFrame(settings, {0, 0, -1});
            const std::array<float, 6> expected{0.25f, 0.75f, 1, 0.125f, 1, 0.25f};
            std::array<VkImage, 2> handles{};
            std::array<SceneOutput, 2> outputs;
            for (size_t i = 0; i < expected.size(); ++i)
            {
                native->pollEvents();
                ASSERT_TRUE(renderer.beginUiFrame());
                const FrameExtent extent{i < 3 ? 192u : 256u, 128};
                auto output = renderer.prepareSceneTarget(extent);
                std::vector<DrawItem> casters;
                if (i != 2 && i != 4)
                {
                    casters.push_back({mesh, {}, glm::translate(glm::mat4(1), {0, 0, -2})});
                    if (i != 1)
                    {
                        const auto model = glm::translate(glm::mat4(1), {0, 0, i == 3 ? 3 : 2}) *
                                           glm::scale(glm::mat4(1), {-0.5f, 1.5f, 0});
                        casters.push_back({i == 3 ? reversed : mesh, {}, model});
                    }
                }
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(output, {extent, {glm::mat4(1), glm::mat4(1)}, {}, {},
                                                  i == 4 ? std::nullopt : shadow, casters},
                                         {0, 0, 0, 1});
                outputs[i % 2] = output;
                if (i == 4)
                    continue;
                Detail::RendererReviewAccess::withShadowOutput(
                    renderer, output, [&](const auto& device, const auto& image) {
                        EXPECT_EQ(image.extent().width, 1024u);
                        EXPECT_EQ(image.mipLevels(), 1u);
                        if (!handles[i % 2])
                            handles[i % 2] = image.handle();
                        EXPECT_EQ(image.handle(), handles[i % 2]);
                        EXPECT_NEAR(readShadowDepth(device, image), expected[i], 0.00003f);
                        EXPECT_NEAR(readShadowDepth(device, image, 0, 0), 1, 0.00003f);
                        if (i == 0)
                        {
                            const double encoded = 1.055 * std::pow(0.25, 1.0 / 2.4) - 0.055;
                            EXPECT_NEAR(sampleShadowDepth(device, image, vertWords, fragWords), encoded * 255, 2);
                        }
                    });
                if (i == 1)
                    Detail::RendererReviewAccess::withShadowOutput(
                        renderer, outputs[0], [&](const auto& device, const auto& image) {
                            EXPECT_NE(handles[0], handles[1]);
                            EXPECT_NEAR(readShadowDepth(device, image), 0.25f, 0.00003f);
                        });
            }

            // 源根平移先于实体旋转与缩放：验证在途深度槽保护与中心深度 0.25
            auto model = Model::prepareModel(renderer, "models/shadow-source.gltf");
            auto srcCasters = expandModel(*model, Scene::Transform({1, 2, 2}, {1, 0, 0, 1}, {2, -3, 0}).matrix());
            for (auto& d : srcCasters)
                d.material = {};
            ShadowSettings srcSettings{true, {2, 68.0f / 3, 0}, {3, 3}, 1, 9, 0.001f};
            for (int i = 0; i < 4; ++i)
            {
                native->pollEvents();
                ASSERT_TRUE(renderer.beginUiFrame());
                auto output = renderer.prepareSceneTarget({256, 128});
                srcSettings.enabled = i != 2;
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(
                    output, {{256, 128}, {glm::mat4(1), glm::mat4(1)}, {}, {}, makeShadowFrame(srcSettings, {0, 0, -1}),
                             srcCasters},
                    {0, 0, 0, 1});
                if (i == 0)
                    Detail::RendererReviewAccess::withShadowOutput(
                        renderer, output, [&](const auto& device, const auto& image) {
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
                EXPECT_THROW(submit(), Error::Exception);
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

    TEST_F(ModelPreparationTest, FixedShowcaseModelsPrepareReleaseCpuAndDrawSharedInstances)
    {
        copyShowcaseModels();
        auto native = window();
        std::vector<std::shared_ptr<const Model::PreparedModel>> models;
        size_t expectedMeshes = 0, expectedTextures = 0, expectedMaterials = 0;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            for (const auto& [path, scale, y] :
                 {std::tuple{"models/utah_teapot/utah_teapot.obj", 0.075f, 0.0f},
                  std::tuple{"models/avocado/glTF/Avocado.gltf", 24.0f, -0.78f}})
            {
                auto prepared = Model::prepareModel(renderer, path);
                ASSERT_FALSE(prepared->draws().empty());
                expectedMeshes += prepared->meshes().size();
                expectedTextures += prepared->textures().size();
                expectedMaterials += prepared->materials().size();
                models.push_back(std::move(prepared));
                released(ResourceType::MODEL, path);
                Scene::Scene comparison;
                comparison.setCamera(Scene::Camera({0, 0, 6}));
                comparison.createEntity(models.back(), Scene::Transform({0, y, 0}, {1, 0, 0, 0}, glm::vec3(scale)));
                comparison.createEntity(models.back(),
                                        Scene::Transform({1000, y, 0}, {1, 0, 0, 0}, glm::vec3(scale)));
                const auto enabled = Scene::extractSceneFrame(comparison, {384, 256});
                const auto disabled = Scene::extractSceneFrame(comparison, {384, 256}, false);
                ASSERT_TRUE(enabled && disabled);
                EXPECT_EQ(enabled->draws.size(), models.back()->draws().size());
                EXPECT_EQ(disabled->draws.size(), models.back()->draws().size() * 2);
                const auto pixels = TextureGpuReview::captureFrame(renderer, enabled->input());
                EXPECT_EQ(pixels, TextureGpuReview::captureFrame(renderer, disabled->input()));
                auto blank = *enabled;
                blank.draws.clear();
                EXPECT_NE(pixels, TextureGpuReview::captureFrame(renderer, blank.input()));
            }
            released(ResourceType::TEXTURE, "models/avocado/glTF/Avocado_baseColor.png");
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), expectedMeshes);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), expectedTextures);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), expectedMaterials);
            EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline:"), 1u);
        }
        for (const auto& model : models)
            EXPECT_FALSE(model->meshes()[0].valid());
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, SharedModelReusesGeometryTexturesAndPreservesOtherCpuOwners)
    {
        auto& manager = ResourceManager::instance();
        auto& registry = ResourceRegistry::instance();
        manager.registerLoader(ResourceType::MODEL, std::make_unique<SharedModelLoader>());
        auto modelHold = manager.hold<ModelResource>(ResourceType::MODEL, "models/shared.gltf");
        auto textureHold = manager.hold<TextureResource>(ResourceType::TEXTURE, PNG);
        ASSERT_TRUE(modelHold && textureHold);
        const auto modelId = modelHold.id();
        const auto textureId = textureHold.id();
        const auto* cpu = modelHold.get<ModelResource>();
        const auto* pixels = textureHold.get<TextureResource>();
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
            modelHold.reset();
            textureHold.reset();
            released(ResourceType::MODEL, "models/shared.gltf");
            released(ResourceType::TEXTURE, PNG);
            for (int frame = 0; frame < 6; ++frame)
            {
                draw(*native, renderer, *model, 2);
                glfwWaitEventsTimeout(0.06);
            }
            const auto image = WindowCapture::capture(*native);
            const auto center = (static_cast<size_t>(image.height / 2) * image.width + image.width / 2) * 4;
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(image.rgba[center + channel], 188, 12);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 1u);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 4u);
        }
        EXPECT_FALSE(model->meshes()[0].valid());
        EXPECT_FALSE(model->materials()[0].valid());
        EXPECT_FALSE(model->textures()[0].valid());
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
                const auto model = policy == 0 ? Model::prepareModel(renderer, "models/shared.gltf")
                                               : Model::prepareModel(renderer, "models/shared.gltf", options[policy]);
                EXPECT_EQ(model->source(), Model::ModelReference("models/shared.gltf", options[policy]));
                models.push_back(model);
                released(ResourceType::MODEL, "models/shared.gltf");
                released(ResourceType::TEXTURE, PNG);
                ASSERT_TRUE(model->bounds());
                EXPECT_EQ(model->bounds()->min, models[0]->bounds()->min);
                EXPECT_EQ(model->bounds()->max, models[0]->bounds()->max);
                EXPECT_EQ(model->meshes().size(), 1u);
                EXPECT_EQ(model->textures().size(), 2u);
                EXPECT_EQ(model->materials().size(), 4u);
                ASSERT_EQ(model->draws().size(), 5u);
                for (bool minify : {false, true})
                {
                    const auto extent = minify ? FrameExtent{1, 1} : FrameExtent{256, 256};
                    const auto transform = glm::translate(glm::mat4(1), glm::vec3(minify ? -0.4f : 0, 0, 0.5f));
                    const std::array draws{DrawItem{model->meshes()[0], model->materials()[0], transform}};
                    std::vector<uint8_t> pixels;
                    Detail::RendererReviewAccess::withFrame(
                        renderer, FrameInput{extent, {glm::mat4(1), glm::mat4(1)}, draws},
                        [&](const auto& device, const auto& pipeline, auto frameSet, auto nativeDraws) {
                            pixels = TextureGpuReview::captureDraws(
                                device, pipeline, frameSet, nativeDraws,
                                {0, 0, float(extent.width), float(extent.height), 0, 1});
                        });
                    const size_t offset = minify ? 0 : (128 * 384 + 128) * 4;
                    const int expected = minify ? (policy == 2 ? 188 : 255) : (policy == 0 ? 255 : 188);
                    for (int channel = 0; channel < 3; ++channel)
                        EXPECT_NEAR(pixels[offset + channel], expected, 2);
                    EXPECT_EQ(pixels[offset + 3], 255);
                }
            }
            EXPECT_FALSE(ResourceRegistry::instance().lookup(ResourceType::TEXTURE, "textures/unused.png").valid());
            EXPECT_EQ(occurrences(messages_.str(), "2x2, format 43, 16 bytes, 2 mips"), 2u);
            EXPECT_EQ(occurrences(messages_.str(), "2x2, format 43, 16 bytes, 1 mips"), 1u);
            EXPECT_EQ(occurrences(messages_.str(), "1x1, format 43, 4 bytes, 1 mips"), 3u);
        }
        for (const auto& model : models)
            EXPECT_FALSE(model->materials()[0].valid());
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, MissingInvalidPngAndMaterialFailureRecoverCleanly)
    {
        auto& manager = ResourceManager::instance();
        auto& registry = ResourceRegistry::instance();
        auto modelHold = manager.hold<ModelResource>(ResourceType::MODEL, GLTF);
        auto otherHold = manager.hold<TextureResource>(ResourceType::TEXTURE, "textures/reference_rgba.png");
        ASSERT_TRUE(modelHold && otherHold);
        const auto modelId = modelHold.id();
        const auto otherId = otherHold.id();
        auto native = window();
        std::shared_ptr<const Model::PreparedModel> recovered;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            // 未准备材质着色器时在纹理上传后失败并回滚资源，补齐着色器后可恢复
            EXPECT_THROW(Model::prepareModel(renderer, GLTF), Error::Exception);
            EXPECT_EQ(Detail::RendererReviewAccess::resourceCounts(renderer).textures, 0u);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 0u);
            released(ResourceType::TEXTURE, PNG);

            shaders(renderer);
            recovered = Model::prepareModel(renderer, GLTF);
            for (int frame = 0; frame < 3; ++frame)
                draw(*native, renderer, *recovered);

            // 缺失/损坏 PNG 应在创建任何 GPU 对象前失败，同时保留外部已有 CPU 持有
            fs::remove(fs::path("assets") / PNG);
            for (bool corrupt : {false, true})
            {
                if (corrupt)
                    std::ofstream(fs::path("assets") / PNG) << "invalid PNG";
                try
                {
                    Model::prepareModel(renderer, GLTF);
                    FAIL() << "Accepted failed dependency";
                }
                catch (const Error::Exception& error)
                {
                    EXPECT_EQ(error.code(), Error::Code::RENDER);
                    EXPECT_NE(error.message().find(corrupt ? "Not a PNG" : "PNG file was not found"),
                              std::string::npos);
                }
                EXPECT_EQ(registry.refCount(modelId), 1u);
                EXPECT_EQ(registry.refCount(otherId), 1u);
            }
        }
        EXPECT_FALSE(recovered->meshes()[0].valid());
        modelHold.reset();
        otherHold.reset();
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, UnusedPartsAndInvalidOptionsCreateNoGpuResources)
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
            EXPECT_TRUE(model->meshes().empty() && model->materials().empty() && model->draws().empty());
            EXPECT_FALSE(model->bounds());
            Scene::Scene scene;
            scene.createEntity(model);
            EXPECT_TRUE(Scene::extractSceneFrame(scene, {400, 400})->draws.empty());
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded"), 0u);
        }
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, SceneRebuildReusesSessionModelsAndPreservesFailures)
    {
        using Model::ModelManager;
        auto native = window();
        std::weak_ptr<const Model::PreparedModel> firstSession;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            ModelManager cache(renderer);
            const Model::ModelReference gltf("models/minimal_gltf/model.gltf");
            const Model::ModelReference obj("models/minimal_obj/model.obj");
            const auto first = cache.resolve(gltf);
            firstSession = first;
            EXPECT_EQ(first, cache.resolve(Model::ModelReference("MODELS/minimal_gltf/../minimal_gltf/MODEL.gltf")));
            const auto second = cache.resolve(obj);
            EXPECT_EQ(cache.size(), 2u);
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
            EXPECT_EQ(Scene::rebuildScene(Scene::readScene(*fileIO_, scenePath), resolver).scene->entities()[0].model,
                      first);
            EXPECT_EQ(Scene::rebuildScene(captured, resolver).scene->entities()[0].model, first);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), uploads);
            const auto oldId = built.scene->entities()[0].id;
            EXPECT_EQ(occurrences(messages_.str(), "Prepared model '"), 2u);
            auto missing = description;
            missing.entities[1].model = Model::ModelReference("models/missing.gltf");
            EXPECT_THROW(Scene::rebuildScene(missing, resolver), Error::Exception);
            EXPECT_NE(built.scene->find(oldId), nullptr);
            EXPECT_EQ(Scene::rebuildScene(description, resolver).scene->entities()[0].model, first);

            const auto sourcePath = fs::path("assets") / gltf.path();
            std::ifstream input(sourcePath, std::ios::binary);
            const std::string sourceText{std::istreambuf_iterator<char>(input), {}};
            input.close();
            std::ofstream(sourcePath) << "{";
            Model::PrepareOptions parseOptions;
            parseOptions.shading = ShadingMode::Lambert;
            EXPECT_THROW(cache.resolve(Model::ModelReference(gltf.path(), parseOptions)), Model::LoadError);
            EXPECT_EQ(cache.size(), 2u);
            draw(*native, renderer, *first);
            std::ofstream(sourcePath) << sourceText;

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

            for (const auto& variant : {Model::PrepareOptions{TextureMipMode::FullChain},
                                        Model::PrepareOptions{{}, {TextureFilter::Linear}},
                                        Model::PrepareOptions{{}, {TextureFilter::Nearest, TextureFilter::Linear}},
                                        Model::PrepareOptions{{}, {TextureFilter::Nearest, TextureFilter::Nearest, MipFilter::Nearest}},
                                        Model::PrepareOptions{{}, {}, ShadingMode::Lambert}})
                EXPECT_NE(cache.resolve(Model::ModelReference(gltf.path(), variant)), first);
            released(ResourceType::MODEL, gltf.path());
            EXPECT_EQ(cache.size(), 8u);
            EXPECT_EQ(cache.collectUnused(), 6u);
            EXPECT_EQ(cache.size(), 2u);
            EXPECT_EQ(occurrences(resourceMessages_.str(), "load:"), 3u);
            draw(*native, renderer, *first);
            EXPECT_TRUE(ResourceManager::instance().hasLoader(ResourceType::MODEL));
        }
        EXPECT_TRUE(firstSession.expired());
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, ModelManagerCollectsAfterInFlightFramesAndHandlesShutdownOrder)
    {
        auto native = window();
        std::unique_ptr<Model::ModelManager> lateCache;
        std::shared_ptr<const Model::PreparedModel> external;
        Mesh lateMesh;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            shadowShader(renderer);
            Model::ModelManager models(renderer);
            auto model = models.resolve(Model::ModelReference(GLTF));
            const auto mesh = model->meshes()[0];
            std::weak_ptr<const Model::PreparedModel> weak = model;
            Scene::Scene scene;
            scene.createEntity(model);
            scene.createEntity(model, Scene::Transform({1000, 0, 0}), false);  // 隐藏实体但仍可作阴影投射示例
            const auto casterOnly = scene.createEntity(model, Scene::Transform({1000, 0, 0}), true);
            (void)casterOnly;
            model.reset();
            EXPECT_EQ(models.collectUnused(), 0u);
            ShadowSettings settings;
            settings.enabled = true;
            const auto frame = Scene::extractSceneFrame(scene, {384, 256}, true, settings);
            ASSERT_TRUE(frame && !frame->shadowDraws.empty());
            SceneOutput output;
            for (int slot = 0; slot != 2; ++slot)
            {
                ASSERT_TRUE(renderer.beginUiFrame());
                output = renderer.prepareSceneTarget(frame->extent);
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(output, frame->input(), {0.2f, 0.24f, 0.28f, 1});
            }
            scene.clear();
            EXPECT_EQ(models.collectUnused(), 1u);
            EXPECT_TRUE(weak.expired());
            EXPECT_TRUE(mesh.valid());  // 飞行帧槽仍保护 GPU 资源
            renderer.prepareSceneTarget(frame->extent);
            EXPECT_TRUE(mesh.valid());
            Detail::RendererReviewAccess::withSceneOutput(renderer, output, [&](const auto& device, const auto& image) {
                EXPECT_EQ(TextureGpuReview::captureMip(device, image, 0, {384, 256}).size(), 384u * 256u * 4u);
            });
            EXPECT_FALSE(mesh.valid());
            EXPECT_EQ(Detail::RendererReviewAccess::resourceCounts(renderer).meshes, 0u);

            // 缓存早于外部使用者析构，以及 Renderer 早于缓存析构均安全
            auto transient = std::make_unique<Model::ModelManager>(renderer);
            external = transient->resolve(Model::ModelReference(GLTF));
            transient.reset();
            EXPECT_TRUE(external->meshes()[0].valid());
            lateCache = std::make_unique<Model::ModelManager>(renderer);
            lateMesh = lateCache->resolve(Model::ModelReference(OBJ))->meshes()[0];
        }
        EXPECT_FALSE(external->meshes()[0].valid());
        EXPECT_FALSE(lateMesh.valid());
        external.reset();
        EXPECT_EQ(lateCache->collectUnused(), 1u);
        lateCache.reset();
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, UnavailablePartBoundsInvalidateWholeModelWithoutDiscardingDraws)
    {
        glm::mat4 projective(1.0f), overflow(1.0f);
        projective[0][3] = 0.5f;
        overflow[0][0] = overflow[3][0] = std::numeric_limits<float>::max();
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            for (const auto& matrix : {projective, overflow})
            {
                ResourceManager::instance().registerLoader(ResourceType::MODEL,
                                                           std::make_unique<TransformModelLoader>(matrix));
                const auto model = Model::prepareModel(renderer, "models/unavailable-bounds.gltf");
                EXPECT_FALSE(model->bounds());
                ASSERT_EQ(model->draws().size(), 5u);
                Scene::Scene scene;
                scene.createEntity(model, Scene::Transform({1000, 0, -10}));
                EXPECT_FALSE(scene.pick(Math::LineSegment{{1000, 0, 0}, {1000, 0, -20}}));
                EXPECT_EQ(Scene::extractSceneFrame(scene, {400, 400})->draws.size(), 5u);
                EXPECT_EQ(model->draws().back().model, glm::translate(glm::mat4(1), glm::vec3(9, 0, 0)));
            }
        }
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, ImportedSnapshotSubmitsAfterSceneClear)
    {
        auto native = window(true);
        Scene::Scene scene;
        scene.setCamera(Scene::Camera({0, 0, 1}, {1, 0, 0, 0}, {glm::radians(90.0f), 0.1f, 10}));
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            const auto gltf = Model::prepareModel(renderer, GLTF);
            const auto obj = Model::prepareModel(renderer, OBJ);
            scene.createEntity(gltf, Scene::Transform({-0.65f, 0.45f, 0}, {1, 0, 0, 0}, glm::vec3(0.25f)));
            scene.createEntity(obj, Scene::Transform({0.65f, -0.55f, 0}, {1, 0, 0, 0}, glm::vec3(0.35f)));
            scene.createEntity();
            released(ResourceType::MODEL, GLTF);
            released(ResourceType::MODEL, OBJ);
            released(ResourceType::TEXTURE, PNG);
            native->pollEvents();
            const auto extent = renderer.beginUiFrame();
            ASSERT_TRUE(extent);
            auto retained = Scene::extractSceneFrame(scene, *extent);
            ASSERT_TRUE(retained && retained->draws.size() == 6u);
            scene.clear();
            renderer.imgui().endFrame();
            renderer.drawFrame(retained->input(), {0.2f, 0.24f, 0.28f, 1});
            retained.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            const auto image = WindowCapture::capture(*native);
            for (const auto& pt : {std::array{-0.510897f, 0.567851f}, std::array{0.86f, -0.608333f}})
            {
                const int px = image.width / 2 + int(pt[0] * image.height / 2);
                const int py = image.height / 2 - int(pt[1] * image.height / 2);
                EXPECT_NEAR(image.rgba[(size_t(py) * image.width + px) * 4 + 0], 188, 12);
                EXPECT_NEAR(image.rgba[(size_t(py) * image.width + px) * 4 + 1], 255, 12);
                EXPECT_NEAR(image.rgba[(size_t(py) * image.width + px) * 4 + 2], 188, 12);
            }
        }
        cleanValidation();
    }

    TEST_F(ModelPreparationTest, LambertInterpolatedNormalsMatchFinalRootNodeEntityInverseTranspose)
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
                ASSERT_TRUE(snapshot && snapshot->draws.size() == 5u);
                std::array draws{snapshot->draws[2]};
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        EXPECT_NEAR(draws[0].model[c][r], expectedModel[c][r], 0.00001f);
                const auto center = glm::vec3(expectedModel * glm::vec4(1.0f / 3, 1.0f / 3, 0, 1));
                float radius = 0;
                for (const auto& pos : positions)
                    radius = std::max(radius, glm::length(glm::vec3(expectedModel * glm::vec4(pos, 1)) - center));
                radius *= 1.3f;
                auto projection = glm::orthoRH_ZO(-radius * 1.5f, radius * 1.5f, -radius, radius, 0.1f, 20.0f);
                projection[1][1] *= -1;
                const CameraMatrices camera{glm::lookAtRH(center + glm::vec3(0, 0, 10), center, glm::vec3(0, 1, 0)),
                                            projection};
                std::array<glm::vec2, 3> screen;
                std::array<glm::dvec3, 3> worldNormals;
                const auto normalMatrix = glm::transpose(glm::inverse(glm::dmat3(expectedModel)));
                Detail::RendererReviewAccess::withFrame(
                    renderer, {{384, 256}, camera, draws, snapshot->lighting},
                    [&](const auto&, const auto&, auto, auto nativeDraws) {
                        ASSERT_EQ(nativeDraws.size(), 1u);
                        for (int c = 0; c < 3; ++c)
                            for (int r = 0; r < 3; ++r)
                                EXPECT_NEAR(nativeDraws[0].object.normalColumns[c][r], normalMatrix[c][r],
                                            0.00001 * std::max(1.0, std::abs(normalMatrix[c][r])));
                    });
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

    TEST_F(ModelPreparationTest, DemoModelsWithUnicodeDependenciesPreserveDirectionAndExactGpuColor)
    {
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
            text.replace(text.find(from), from.size(), to);
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
            const std::array<std::array<int, 4>, 12> direction = {{{255, 255, 255, 255}, {255, 128, 0, 255},
                                                                   {0, 0, 0, 255},       {0, 0, 255, 255},
                                                                   {128, 128, 128, 255}, {255, 0, 0, 255},
                                                                   {0, 255, 0, 255},     {255, 0, 255, 255},
                                                                   {0, 255, 255, 255},   {17, 34, 51, 255},
                                                                   {255, 255, 0, 255},   {128, 0, 255, 255}}};
            auto ortho = glm::orthoRH_ZO(-1.5f, 1.5f, -1.0f, 1.0f, 0.1f, 10.0f);
            ortho[1][1] *= -1;
            const CameraMatrices cam{glm::lookAtRH(glm::vec3(0, 0, 3), glm::vec3(0), glm::vec3(0, 1, 0)), ortho};
            for (const auto& model : models)
            {
                const auto draws = expandModel(*model, glm::inverse(model->draws()[0].model));
                const auto pixels = TextureGpuReview::captureFrame(renderer, {{384, 256}, cam, draws});
                ASSERT_EQ(pixels.size(), 384u * 256 * 4);
                for (int row = 0; row < 4; ++row)
                    for (int col = 0; col < 3; ++col)
                    {
                        const float x = -1 + (col + 0.5f) / 3, y = 0.5f - (row + 0.5f) / 4;
                        const size_t idx = (size_t((1 - y) * 128) * 384 + size_t((x / 1.5f + 1) * 192)) * 4;
                        for (size_t c = 0; c < 4; ++c)
                            EXPECT_NEAR(pixels[idx + c], direction[row * 3 + col][c], c == 3 ? 0 : 1);
                    }
                const size_t tintIdx = (size_t((1 + 1.0f / 6) * 128) * 384 + size_t((0.6f / 1.5f + 1) * 192)) * 4;
                EXPECT_NEAR(pixels[tintIdx + 0], 188, 1);
                EXPECT_NEAR(pixels[tintIdx + 1], 255, 1);
                EXPECT_NEAR(pixels[tintIdx + 2], 188, 1);
            }
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 4u);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 4u);
        }
        cleanValidation();
    }
}  // namespace
