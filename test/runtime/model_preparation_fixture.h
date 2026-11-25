#pragma once

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
#include "runtime/resource/asset_importer.h"
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


namespace Incubator::Test
{
    using namespace Incubator;
    using namespace Incubator::Resource;
    using namespace Incubator::Render;
    namespace fs = std::filesystem;
    constexpr const char* GLTF = "models/minimal_gltf/model.gltf";
    constexpr const char* OBJ = "models/minimal_obj/model.obj";
    constexpr const char* PNG = "textures/direction_rgba.png";

    inline size_t occurrences(std::string_view text, std::string_view value)
    {
        size_t count = 0;
        for (auto at = text.find(value); at != std::string_view::npos; at = text.find(value, at + value.size()))
            ++count;
        return count;
    }

    inline float readShadowDepth(const Vulkan::VulkanDevice& device, const Vulkan::VulkanImage& image, uint32_t x = 512,
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

    inline uint8_t sampleShadowDepth(const Vulkan::VulkanDevice& device, const Vulkan::VulkanImage& image,
                              const std::vector<uint32_t>& vertex, const std::vector<uint32_t>& fragment)
    {
        // 复用正式纹理 Shader 的 Unlit 路径验证真实片元采样，只取深度的 R 分量（阴影图为 2D Array，为第 0 层创建临时 2D 视图）。
        const auto frame = Vulkan::makeFrameUniform({{384, 256}, {glm::mat4(1), glm::mat4(1)}, {}});
        const std::array uniforms{std::make_shared<Vulkan::VulkanBuffer>(
            device, sizeof(frame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        uniforms[0]->write(0, std::as_bytes(std::span(&frame, 1)));
        Vulkan::VulkanBindingSet bindings(device, uniforms);
        Vulkan::VulkanSampler sampler(device, {});
        Vulkan::VulkanMaterialBinding material(device, bindings.materialLayout(), bindings.normalFallback(), sampler,
                                               bindings.normalFallback(), sampler,
                                               bindings.metallicRoughnessFallback(), sampler,
                                               bindings.emissiveFallback(), sampler, {});
        VkImageViewCreateInfo layerViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        layerViewInfo.image = image.handle();
        layerViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        layerViewInfo.format = image.format();
        layerViewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VkImageView layerView = VK_NULL_HANDLE;
        if (vkCreateImageView(device.handle(), &layerViewInfo, nullptr, &layerView) != VK_SUCCESS)
            throw Error::Exception(Error::Code::RENDER, "vkCreateImageView(sampleShadowDepth) failed");
        try
        {
            const VkDescriptorImageInfo imageInfo{sampler.handle(), layerView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             nullptr,
                                             material.descriptorSet(),
                                             0,
                                             0,
                                             1,
                                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             &imageInfo,
                                             nullptr,
                                             nullptr};
            vkUpdateDescriptorSets(device.handle(), 1, &write, 0, nullptr);

            Vulkan::VulkanPipeline pipeline(device, VK_FORMAT_R8G8B8A8_UNORM, device.findDepthFormat(),
                                            bindings.layout(), bindings.materialLayout(), vertex, fragment);
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
            vkDestroyImageView(device.handle(), layerView, nullptr);
            return pixels[(128 * 384 + 192) * 4];
        }
        catch (...)
        {
            vkDestroyImageView(device.handle(), layerView, nullptr);
            throw;
        }
    }

    inline Scene::Camera shadowReviewCamera(glm::vec3 eye = {0, -3, 3})
    {
        const auto view = glm::lookAtRH(eye, glm::vec3(0, 0.5f, 0), glm::vec3(0, 1, 0));
        return Scene::Camera(eye, glm::quat_cast(glm::mat3(glm::inverse(view))));
    }

    inline void expectShadowPixel(const Scene::SceneFrame& frame, std::span<const uint8_t> pixels, glm::vec3 world,
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
                const auto displayVertex = manager.hold<ShaderResource>(ResourceType::SHADER, "shaders/display.vert.spv");
                const auto displayFragment =
                    manager.hold<ShaderResource>(ResourceType::SHADER, "shaders/display.frag.spv");
                ASSERT_TRUE(vertex);
                ASSERT_TRUE(fragment);
                ASSERT_TRUE(displayVertex);
                ASSERT_TRUE(displayFragment);
                renderer.prepareSceneResources(*vertex.get<ShaderResource>(), *fragment.get<ShaderResource>());
                renderer.prepareDisplayResources(*displayVertex.get<ShaderResource>(),
                                                 *displayFragment.get<ShaderResource>());
            }
            released(ResourceType::SHADER, "shaders/cube.vert.spv");
            released(ResourceType::SHADER, "shaders/cube.frag.spv");
            released(ResourceType::SHADER, "shaders/display.vert.spv");
            released(ResourceType::SHADER, "shaders/display.frag.spv");
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
            auto input = frame.input();
            input.display = {ToneMappingMode::Off, 0.0f};
            renderer.drawSceneTarget(output, input, {0.12f, 0.16f, 0.22f, 1});
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
}  // namespace Incubator::Test
