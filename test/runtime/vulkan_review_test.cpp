#include <GLFW/glfw3.h>
#include <gtest/gtest.h>
#include <imgui.h>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glm/ext/matrix_transform.hpp>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/platform/window/window.h"
#include "runtime/render/detail/renderer_review.h"
#include "runtime/render/integration/imgui_renderer.h"
#include "runtime/render/renderer.h"
#include "runtime/render/vulkan/glfw_surface.h"
#include "runtime/render/vulkan/vulkan_device.h"
#include "runtime/render/vulkan/vulkan_image.h"
#include "runtime/render/vulkan/vulkan_instance.h"
#include "runtime/render/vulkan/vulkan_swapchain.h"
#include "runtime/resource/loader/shader_loader.h"
#include "runtime/resource/loader/shader_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"
#include "support/render_demo/scene_assets.h"
#include "support/render_demo/scene_frame.h"
#include "support/render_demo/scene_mesh.h"
#include "support/texture_gpu_review.h"
#include "support/window_capture.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Render;
    using namespace Incubator::Render::Vulkan;

    glm::vec3 linearFromSrgb(glm::vec3 value)
    {
        for (int channel = 0; channel < 3; ++channel)
            value[channel] = value[channel] <= 0.04045f ? value[channel] / 12.92f
                                                        : std::pow((value[channel] + 0.055f) / 1.055f, 2.4f);
        return value;
    }

    void expectLinearPixel(std::span<const uint8_t> pixels, glm::vec3 linear, uint32_t x = 192, uint32_t y = 128,
                           uint32_t width = 384)
    {
        const size_t offset = (size_t(y) * width + x) * 4;
        ASSERT_LT(offset + 3, pixels.size());
        for (int channel = 0; channel < 3; ++channel)
        {
            const double value = std::clamp(double(linear[channel]), 0.0, 1.0);
            const double encoded = value <= 0.0031308 ? 12.92 * value : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
            EXPECT_NEAR(pixels[offset + channel], encoded * 255, 2) << "channel=" << channel;
        }
        EXPECT_EQ(pixels[offset + 3], 255);
    }

    std::vector<const char*> requiredExtensions()
    {
        auto result = GlfwSurface::requiredInstanceExtensions();
        result.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        result.push_back(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        return result;
    }

    size_t countOccurrences(std::string_view messages, std::string_view value)
    {
        size_t count = 0;
        for (size_t position = messages.find(value); position != std::string_view::npos;
             position = messages.find(value, position + value.size()))
            ++count;
        return count;
    }

    class EmptyLayerSearch
    {
    public:
        EmptyLayerSearch()
        {
            const auto empty = std::filesystem::absolute("review/empty-layers");
            std::filesystem::create_directories(empty);
            for (size_t index = 0; index < names_.size(); ++index)
            {
                const auto* current = std::getenv(names_[index]);
                previous_[index] = current ? current : "";
                _putenv_s(names_[index], empty.string().c_str());
            }
        }
        ~EmptyLayerSearch()
        {
            for (size_t index = 0; index < names_.size(); ++index)
                _putenv_s(names_[index], previous_[index].c_str());
        }

    private:
        const std::array<const char*, 2> names_ = {"VK_LAYER_PATH", "VK_IMPLICIT_LAYER_PATH"};
        std::array<std::string, 2> previous_;
    };

    using SceneAssets = Demo::SceneAssets;

    Demo::SceneAssetPaths referenceSceneAssetPaths()
    {
        Demo::SceneAssetPaths paths;
        paths.texture = "textures/reference_rgba.png";
        return paths;
    }

    struct FileAudit
    {
        Demo::SceneAssetPaths paths;
        size_t textureOpens = 0;
        bool cpuReleasedBeforeDestroy = false;
    };

    class AuditedFileIO : public FileIO::FileIO
    {
    public:
        explicit AuditedFileIO(FileAudit& audit) : audit_(audit), inner_(Incubator::FileIO::createPlatformFileIO()) {}
        ~AuditedFileIO()
        {
            auto& registry = Resource::ResourceRegistry::instance();
            audit_.cpuReleasedBeforeDestroy = true;
            for (const auto& [type, path] : {std::pair{Resource::ResourceType::SHADER, audit_.paths.vertex},
                                             std::pair{Resource::ResourceType::SHADER, audit_.paths.fragment},
                                             std::pair{Resource::ResourceType::TEXTURE, audit_.paths.texture}})
            {
                const auto id = registry.lookup(type, path);
                if (id.valid() && (registry.refCount(id) != 0 || registry.getObject(id)))
                    audit_.cpuReleasedBeforeDestroy = false;
            }
        }
        bool exists(const std::filesystem::path& path) override
        {
            return inner_->exists(path);
        }
        uint64_t getFileSize(const std::filesystem::path& path) override
        {
            return inner_->getFileSize(path);
        }
        bool remove(const std::filesystem::path& path) override
        {
            return inner_->remove(path);
        }
        std::unique_ptr<Incubator::FileIO::FileHandle> open(const std::filesystem::path& path,
                                                            Incubator::FileIO::OpenMode mode,
                                                            Incubator::FileIO::ShareMode share) override
        {
            if (path.extension() == ".png")
                ++audit_.textureOpens;
            return inner_->open(path, mode, share);
        }
        std::vector<Incubator::FileIO::DirectoryEntry> listDirectory(const std::filesystem::path& path) override
        {
            return inner_->listDirectory(path);
        }
        bool directoryExists(const std::filesystem::path& path) override
        {
            return inner_->directoryExists(path);
        }
        bool createDirectory(const std::filesystem::path& path) override
        {
            return inner_->createDirectory(path);
        }
        bool removeDirectory(const std::filesystem::path& path) override
        {
            return inner_->removeDirectory(path);
        }

    private:
        FileAudit& audit_;
        std::unique_ptr<Incubator::FileIO::FileIO> inner_;
    };

    void expectReleased(Demo::SceneAssetPaths paths)
    {
        auto& registry = Resource::ResourceRegistry::instance();
        for (const auto& [type, path] : {std::pair{Resource::ResourceType::SHADER, paths.vertex},
                                         std::pair{Resource::ResourceType::SHADER, paths.fragment},
                                         std::pair{Resource::ResourceType::TEXTURE, paths.texture}})
        {
            const auto id = registry.lookup(type, path);
            if (id.valid())
            {
                EXPECT_EQ(registry.refCount(id), 0u);
                EXPECT_EQ(registry.getObject(id), nullptr);
            }
        }
        EXPECT_FALSE(Resource::ResourceManager::instance().hasLoader(Resource::ResourceType::SHADER));
        EXPECT_FALSE(Resource::ResourceManager::instance().hasLoader(Resource::ResourceType::TEXTURE));
    }

    void releaseCpuAssets(SceneAssets& assets)
    {
        const std::array ids = {assets.vertex->id(), assets.fragment->id(), assets.texture->id()};
        assets.releaseCpuResources();
        for (auto id : ids)
        {
            EXPECT_EQ(Resource::ResourceRegistry::instance().refCount(id), 0u);
            EXPECT_EQ(Resource::ResourceRegistry::instance().getObject(id), nullptr);
        }
        EXPECT_FALSE(Resource::ResourceManager::instance().hasLoader(Resource::ResourceType::SHADER));
        EXPECT_FALSE(Resource::ResourceManager::instance().hasLoader(Resource::ResourceType::TEXTURE));
    }

    // 模拟应用启动时帧缓冲不可用的情况，不对 Vulkan 做模拟。
    class DeferredFramebufferWindow final : public Window::Window
    {
    public:
        explicit DeferredFramebufferWindow(Incubator::Window::Window& native) : native_(native) {}
        void beginInputFrame() override { native_.beginInputFrame(); }
        const Input::InputState& input() const override { return native_.input(); }
        bool shouldClose() const override
        {
            return native_.shouldClose();
        }
        void setShouldClose(bool value) override
        {
            native_.setShouldClose(value);
        }
        void pollEvents() override
        {
            native_.pollEvents();
        }
        void getFramebufferSize(int& width, int& height) const override
        {
            if (throwOnSize)
                throw Error::Exception(Error::Code::RENDER, "Injected framebuffer query failure");
            if (drawable)
                native_.getFramebufferSize(width, height);
            else
                width = height = 0;
        }
        void getWindowSize(int& width, int& height) const override
        {
            native_.getWindowSize(width, height);
        }
        void setTitle(std::string_view title) override
        {
            native_.setTitle(title);
        }
        bool isIconified() const override
        {
            return native_.isIconified();
        }
        float getContentScale() const override
        {
            return native_.getContentScale();
        }
        void* getNativeHandle() const override
        {
            return native_.getNativeHandle();
        }
        bool drawable = false;
        bool throwOnSize = false;

    private:
        Incubator::Window::Window& native_;
    };

    class VulkanReviewTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            previousPath_ = std::filesystem::current_path();
            std::filesystem::current_path(Utils::Utf8::toFsPath(RUNTIME_TEST_ASSET_DIR).parent_path());
            Log::Registry::reset();
            Resource::ResourceManager::instance().reset();
            Resource::ResourceRegistry::instance().clear();
            auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(messages_);
            sink->set_pattern("%l|%v");
            Log::Registry::get("render.vulkan").addSink(sink);
            Log::Registry::get("render").addSink(sink);
        }

        void TearDown() override
        {
            Log::Registry::reset();
            Resource::ResourceManager::instance().reset();
            Resource::ResourceRegistry::instance().clear();
            std::filesystem::current_path(previousPath_);
        }

        std::unique_ptr<Window::Window> createWindow(bool visible = false, int width = 640, int height = 360)
        {
            Window::WindowDesc desc;
            desc.title = "Incubator Vulkan review";
            desc.width = width;
            desc.height = height;
            desc.visible = visible;
            return Window::createPlatformWindow(desc);
        }

        Mesh createCubeMesh(Renderer& renderer)
        {
            const auto data = Demo::makeCubeMesh();
            return renderer.createMesh(data.vertices, data.indices);
        }

        int drawUiFrame(Window::Window& window, Renderer& renderer, const Mesh& mesh, const Material& material)
        {
            window.pollEvents();
            const auto extent = renderer.beginUiFrame();
            if (!extent)
                return 0;
            ImGui::Begin("Vulkan review");
            ImGui::Text("Textured cube and UI lifecycle verification");
            ImGui::End();
            renderer.imgui().endFrame();
            auto* data = renderer.imgui().drawData();
            int recorded = 0;
            const auto scale = glm::scale(glm::mat4(1.0f), glm::vec3(0.6f));
            const std::array draws = {
                DrawItem{mesh, material, glm::translate(glm::mat4(1.0f), glm::vec3(-0.65f, 0.0f, 0.0f)) * scale},
                DrawItem{mesh, material, glm::translate(glm::mat4(1.0f), glm::vec3(0.65f, 0.0f, 0.0f)) * scale}};
            renderer.drawFrame(Demo::makeSceneFrame(*extent, draws), {0.12f, 0.16f, 0.22f, 1.0f}, [&](void* commands) {
                ++recorded;
                renderer.imgui().recordDrawData(data, commands);
            });
            return recorded;
        }

        FrameInput controlledFrame(FrameExtent extent, std::span<const DrawItem> draws)
        {
            const float aspect = static_cast<float>(extent.width) / extent.height;
            auto projection = glm::orthoRH_ZO(-aspect, aspect, -1.0f, 1.0f, 0.1f, 10.0f);
            projection[1][1] *= -1.0f;
            const auto view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            return {extent, {view, projection}, draws};
        }

        FrameExtent submitControlledScene(Window::Window& window, Renderer& renderer, std::span<const DrawItem> draws,
                                          bool withUi = false)
        {
            window.pollEvents();
            const auto extent = renderer.beginUiFrame();
            if (!extent)
                throw Error::Exception(Error::Code::RENDER, "Controlled scene requires a drawable frame");
            if (withUi)
                ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(16, 16), ImVec2(48, 48),
                                                              IM_COL32(255, 0, 255, 255));
            renderer.imgui().endFrame();
            auto* ui = renderer.imgui().drawData();
            renderer.drawFrame(controlledFrame(*extent, draws), {0.2f, 0.24f, 0.28f, 1.0f}, [&](void* commands) {
                if (withUi)
                    renderer.imgui().recordDrawData(ui, commands);
            });
            return *extent;
        }

        void expectCleanValidation()
        {
            const auto messages = messages_.str();
            std::cout << messages;
            EXPECT_NE(messages.find("validation: enabled"), std::string::npos);
            EXPECT_EQ(messages.find("warning|"), std::string::npos) << messages;
            EXPECT_EQ(messages.find("error|"), std::string::npos) << messages;
        }

        std::filesystem::path previousPath_;
        std::ostringstream messages_;
    };

    TEST_F(VulkanReviewTest, RendererPresentsEmptyUiBeforeExplicitScenePreparation)
    {
        Log::Registry::get("render.vulkan").setLevel(Log::Level::DEBUG);
        auto window = createWindow(true, 640, 480);
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            EXPECT_FALSE(Resource::ResourceManager::instance().hasLoader(Resource::ResourceType::SHADER));
            EXPECT_FALSE(Resource::ResourceManager::instance().hasLoader(Resource::ResourceType::TEXTURE));
            for (int frame = 0; frame < 6; ++frame)
            {
                submitControlledScene(*window, renderer, {}, true);
                glfwWaitEventsTimeout(0.025);
            }
            const auto empty = WindowCapture::capture(*window);
            const auto pixel = [&](int x, int y, int channel) {
                return empty.rgba[(static_cast<size_t>(y) * empty.width + x) * 4 + channel];
            };
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_EQ(pixel(320, 240, channel), pixel(8, 8, channel));
            EXPECT_GT(pixel(24, 24, 0), 220);
            EXPECT_LT(pixel(24, 24, 1), 30);
            EXPECT_GT(pixel(24, 24, 2), 220);
            EXPECT_EQ(countOccurrences(messages_.str(), "Uploaded RGBA8 texture"), 0u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline"), 0u);
            WindowCapture::save(empty, "review/resource-entry-empty-ui.ppm");

            SceneAssets assets;
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            const auto data = Demo::makePlaneMesh();
            const auto mesh = renderer.createMesh(data.vertices, data.indices);
            releaseCpuAssets(assets);
            const std::array draws = {DrawItem{mesh, material, glm::mat4(1.0f)}};
            for (int frame = 0; frame < 6; ++frame)
            {
                submitControlledScene(*window, renderer, draws);
                glfwWaitEventsTimeout(0.025);
            }
            const auto scene = WindowCapture::capture(*window);
            const auto bounds = WindowCapture::bounds(scene);
            EXPECT_GT(bounds[2] - bounds[0], 100);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 1u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline"), 1u);
            WindowCapture::save(scene, "review/resource-entry-prepared.ppm");
        }
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, UInt32IndexDrawUsesVertexAboveUint16Range)
    {
        auto window = createWindow(true, 640, 480);
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            std::vector<MeshVertex> vertices(65537, MeshVertex{{4.0f, 4.0f, 0.0f}, {0.0f, 0.0f}});
            vertices[0] = {{-0.65f, -0.55f, 0.0f}, {0.0f, 0.0f}};
            vertices[1] = {{0.65f, -0.55f, 0.0f}, {1.0f, 0.0f}};
            vertices[65536] = {{0.0f, 0.65f, 0.0f}, {0.5f, 1.0f}};
            const std::array<uint32_t, 3> indices = {0, 1, 65536};
            const auto mesh = renderer.createMesh(vertices, indices);
            releaseCpuAssets(assets);
            const std::array draws = {DrawItem{mesh, material, glm::mat4(1.0f)}};
            for (int frame = 0; frame < 6; ++frame)
            {
                submitControlledScene(*window, renderer, draws);
                glfwWaitEventsTimeout(0.025);
            }
            const auto image = WindowCapture::capture(*window);
            const auto bounds = WindowCapture::bounds(image);
            EXPECT_GT(bounds[2] - bounds[0], 100);
            EXPECT_GT(bounds[3] - bounds[1], 100);
            WindowCapture::save(image, "review/uint32-index-high-vertex.ppm");
            std::cout << "GPU_UINT32_INDEX: vertices=" << vertices.size() << " high_index=" << indices.back()
                      << " bounds=" << bounds[0] << ',' << bounds[1] << ',' << bounds[2] << ',' << bounds[3] << '\n';
        }
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, IndependentTexturesShareUploadsAndExpireAfterRendererMoveAndClose)
    {
        Log::Registry::get("render.vulkan").setLevel(Log::Level::DEBUG);
        auto window = createWindow(true, 640, 480);
        Texture reference;
        Texture direction;
        const std::string texturePath = "textures/资源入口 方向 🧪.png";
        struct TemporaryTexture
        {
            std::filesystem::path path;
            ~TemporaryTexture()
            {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        } temporary{Utils::Utf8::toFsPath("assets/" + texturePath)};
        std::filesystem::copy_file("assets/textures/direction_rgba.png", temporary.path,
                                   std::filesystem::copy_options::overwrite_existing);
        FileAudit firstAudit;
        FileAudit secondAudit;
        secondAudit.paths.texture = texturePath;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            Material material;
            {
                SceneAssets assets({}, std::make_unique<AuditedFileIO>(firstAudit));
                auto texture = renderer.createTexture(*assets.texture);
                reference = texture;
                texture = {};
                releaseCpuAssets(assets);
            }
            EXPECT_TRUE(firstAudit.cpuReleasedBeforeDestroy);
            EXPECT_TRUE(reference.valid());
            {
                SceneAssets assets(secondAudit.paths, std::make_unique<AuditedFileIO>(secondAudit));
                direction = renderer.createTexture(*assets.texture);
                const auto shared = direction;
                renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
                material = renderer.createMaterial({shared});
                direction = {};
                direction = shared;
                releaseCpuAssets(assets);
            }
            EXPECT_TRUE(secondAudit.cpuReleasedBeforeDestroy);
            EXPECT_TRUE(reference.valid());
            EXPECT_TRUE(direction.valid());
            EXPECT_FALSE(Texture{}.valid());
            Renderer moved(std::move(renderer));
            Resource::TextureResource cpuTexture({}, 1, 1, {255, 255, 255, 255});
            EXPECT_THROW(renderer.createTexture(cpuTexture), Error::Exception);
            const auto data = Demo::makePlaneMesh();
            const auto mesh = moved.createMesh(data.vertices, data.indices);
            const std::array draws = {DrawItem{mesh, material, glm::mat4(1.0f)}};
            for (int frame = 0; frame < 6; ++frame)
            {
                submitControlledScene(*window, moved, draws);
                glfwWaitEventsTimeout(0.025);
            }
            const auto image = WindowCapture::capture(*window);
            const auto bounds = WindowCapture::bounds(image);
            ASSERT_GT(bounds[2] - bounds[0], 100);
            const int x = bounds[0] + (bounds[2] - bounds[0]) / 6;
            const int y = bounds[1] + (bounds[3] - bounds[1]) / 8;
            const size_t offset = (static_cast<size_t>(y) * image.width + x) * 4;
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_GT(image.rgba[offset + channel], 220);  // 方向纹理以白色作为初始值。
            WindowCapture::save(image, "review/resource-entry-independent-textures.ppm");
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline"), 1u);
            EXPECT_EQ(firstAudit.textureOpens, 1u);
            EXPECT_EQ(secondAudit.textureOpens, 1u);
            // 不显式等待：关闭操作会保护最后一次提交，并释放两张纹理。
        }
        EXPECT_FALSE(reference.valid());
        EXPECT_FALSE(direction.valid());
        expectReleased({});
        expectReleased(secondAudit.paths);
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, ScenePreparationRejectsInvalidReferencesAndCanRetry)
    {
        auto window = createWindow();
        SceneAssets assets;
        Texture expired;
        {
            Renderer previous(*window, RendererValidationMode::Required);
            expired = previous.createTexture(*assets.texture);
        }
        EXPECT_FALSE(expired.valid());
        Texture observer;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            auto texture = renderer.createTexture(*assets.texture);
            observer = texture;
            Resource::ShaderResource emptyShader({}, {});
            EXPECT_THROW(renderer.createMaterial({texture}), Error::Exception);
            EXPECT_THROW(renderer.prepareSceneResources(emptyShader, *assets.fragment), Error::Exception);
            EXPECT_THROW(renderer.prepareSceneResources(*assets.vertex, emptyShader), Error::Exception);
            const auto mesh = createCubeMesh(renderer);
            auto extent = renderer.beginUiFrame();
            ASSERT_TRUE(extent);
            renderer.imgui().endFrame();
            const std::array draws = {DrawItem{mesh, {}, glm::mat4(1.0f)}};
            int recorded = 0;
            EXPECT_THROW(renderer.drawFrame(controlledFrame(*extent, draws), {0.2f, 0.24f, 0.28f, 1.0f},
                                            [&](void*) { ++recorded; }),
                         Error::Exception);
            EXPECT_EQ(recorded, 0);
            renderer.drawFrame(controlledFrame(*extent, {}), {0.2f, 0.24f, 0.28f, 1.0f});
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            EXPECT_THROW(renderer.createMaterial({}), Error::Exception);
            EXPECT_THROW(renderer.createMaterial({expired}), Error::Exception);
            const auto material = renderer.createMaterial({texture});
            EXPECT_THROW(renderer.prepareSceneResources(*assets.vertex, *assets.fragment), Error::Exception);
            texture = {};
            releaseCpuAssets(assets);
            EXPECT_TRUE(observer.valid());
            EXPECT_GT(drawUiFrame(*window, renderer, mesh, material), 0);
        }
        EXPECT_FALSE(observer.valid());
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, SamplerMaterialsShareOneImageAndIndependentlyFilterMinificationAndMagnification)
    {
        Log::Registry::get("render").setLevel(Log::Level::DEBUG);
        Log::Registry::get("render.vulkan").setLevel(Log::Level::DEBUG);
        auto window = createWindow(false, 640, 480);
        SceneAssets assets(referenceSceneAssetPaths());
        Texture observer;
        std::array<Material, 5> materials;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            {
                Resource::TextureResource pixels({}, 2, 2,
                                                 {0, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 0, 255, 255, 255, 255});
                observer = renderer.createTexture(pixels);
            }
            MaterialDesc desc{observer};
            EXPECT_EQ(desc.sampler, SamplerDesc{});
            materials[0] = renderer.createMaterial(desc);
            desc.sampler = {TextureFilter::Nearest, TextureFilter::Linear, MipFilter::BaseLevel};
            materials[1] = renderer.createMaterial(desc);
            desc.sampler = {TextureFilter::Linear, TextureFilter::Nearest, MipFilter::BaseLevel};
            materials[2] = renderer.createMaterial(desc);
            desc.sampler = {TextureFilter::Linear, TextureFilter::Linear, MipFilter::Nearest};
            materials[3] = renderer.createMaterial(desc);
            desc.sampler.mipFilter = MipFilter::Linear;
            materials[4] = renderer.createMaterial(desc);
            desc.sampler = {};
            desc.color = {0, 0, 0};
            auto invalid = desc;
            invalid.sampler.minFilter = static_cast<TextureFilter>(99);
            EXPECT_THROW(renderer.createMaterial(invalid), Error::Exception);
            invalid = desc;
            invalid.sampler.mipFilter = static_cast<MipFilter>(99);
            EXPECT_THROW(renderer.createMaterial(invalid), Error::Exception);
            releaseCpuAssets(assets);
            for (bool minify : {false, true})
            {
                const float u0 = minify ? -128.0f : 0.5f;
                const float u1 = minify ? 0.0f : 0.5f;
                const std::array<MeshVertex, 4> vertices{{{{-1, -1, 0.5f}, {u0, 0.5f}},
                                                          {{0, -1, 0.5f}, {u1, 0.5f}},
                                                          {{0, 1, 0.5f}, {u1, 0.5f}},
                                                          {{-1, 1, 0.5f}, {u0, 0.5f}}}};
                const std::array<uint32_t, 6> indices{0, 1, 2, 2, 3, 0};
                const auto mesh = renderer.createMesh(vertices, indices);
                for (size_t i = 1; i < materials.size(); ++i)
                {
                    SCOPED_TRACE(minify ? "minification" : "magnification");
                    SCOPED_TRACE(i);
                    const std::array draws{
                        DrawItem{mesh, materials[0], glm::mat4(1)},
                        DrawItem{mesh, materials[i], glm::translate(glm::mat4(1), glm::vec3(1, 0, 0))}};
                    const FrameInput input{{256, 256}, {glm::mat4(1), glm::mat4(1)}, draws};
                    std::vector<uint8_t> pixels;
                    Detail::RendererReviewAccess::withFrame(
                        renderer, input,
                        [&](const auto& device, const auto& pipeline, auto frameSet, auto nativeDraws) {
                            pixels = TextureGpuReview::captureDraws(device, pipeline, frameSet, nativeDraws,
                                                                    {0, 0, 256, 256, 0, 1});
                        });
                    ASSERT_EQ(pixels.size(), 384u * 256 * 4);
                    const bool linear = i >= 3 || (minify ? i == 2 : i == 1);
                    for (int channel = 0; channel < 3; ++channel)
                    {
                        EXPECT_NEAR(pixels[(128 * 384 + 64) * 4 + channel], 255, 1);
                        EXPECT_NEAR(pixels[(128 * 384 + 192) * 4 + channel], linear ? 188 : 255, 1);
                    }
                    EXPECT_EQ(pixels[(128 * 384 + 192) * 4 + 3], 255);
                    WindowCapture::save(
                        {384, 256, pixels},
                        std::string("review/sampler-") + (minify ? "min-" : "mag-") + std::to_string(i) + ".ppm");
                }
            }
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 1u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline:"), 1u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created material:"), 5u);
        }
        EXPECT_FALSE(observer.valid());
        for (const auto& material : materials)
            EXPECT_FALSE(material.valid());
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, GeneratedMipExtentsAndLinearSrgbAlphaMatchReadback)
    {
        auto window = createWindow();
        {
            VulkanInstance instance(requiredExtensions(), ValidationMode::Required);
            GlfwSurface surface(instance, *window);
            VulkanDevice device(instance, surface);
            const std::vector<std::vector<VkExtent2D>> chains{{{1, 1}},
                                                              {{1, 9}, {1, 4}, {1, 2}, {1, 1}},
                                                              {{9, 1}, {4, 1}, {2, 1}, {1, 1}},
                                                              {{3, 5}, {1, 2}, {1, 1}},
                                                              {{4, 4}, {2, 2}, {1, 1}}};
            for (const auto& chain : chains)
            {
                SCOPED_TRACE(std::to_string(chain[0].width) + "x" + std::to_string(chain[0].height));
                std::shared_ptr<VulkanImage> image;
                {
                    std::vector<uint8_t> pixels(chain[0].width * chain[0].height * 4);
                    for (size_t i = 0; i < pixels.size(); ++i)
                        pixels[i] = std::array<uint8_t, 4>{64, 128, 192, 85}[i % 4];
                    image = VulkanImage::uploadRgba8(device, chain[0], VK_FORMAT_R8G8B8A8_SRGB, pixels,
                                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                                     TextureMipMode::FullChain);
                }
                ASSERT_EQ(image->mipLevels(), chain.size());
                for (uint32_t level = 0; level < chain.size(); ++level)
                {
                    SCOPED_TRACE(level);
                    const auto pixels = TextureGpuReview::captureMip(device, *image, level, chain[level]);
                    ASSERT_EQ(pixels.size(), chain[level].width * chain[level].height * 4);
                    for (size_t i = 0; i < pixels.size(); ++i)
                        EXPECT_NEAR(pixels[i], (std::array<uint8_t, 4>{64, 128, 192, 85}[i % 4]), 1);
                }
            }
            const std::array<uint8_t, 16> blackWhite{0, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 0, 255, 255, 255, 255};
            const auto image = VulkanImage::uploadRgba8(device, {2, 2}, VK_FORMAT_R8G8B8A8_SRGB, blackWhite,
                                                        VK_IMAGE_USAGE_SAMPLED_BIT, TextureMipMode::FullChain);
            ASSERT_EQ(image->mipLevels(), 2u);
            const auto mip = TextureGpuReview::captureMip(device, *image, 1, {1, 1});
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(mip[channel], 188, 1);
            EXPECT_NEAR(mip[3], 128, 1);  // 直通 alpha 直接求平均，不进行 sRGB 转换或预乘。
            const auto baseOnly = VulkanImage::uploadRgba8(device, {2, 2}, VK_FORMAT_R8G8B8A8_SRGB, blackWhite,
                                                           VK_IMAGE_USAGE_SAMPLED_BIT);
            EXPECT_EQ(baseOnly->mipLevels(), 1u);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, MipMaterialsSelectBaseNearestAndLinearLevelsAfterCpuRelease)
    {
        Log::Registry::get("render.vulkan").setLevel(Log::Level::DEBUG);
        auto window = createWindow(false, 640, 480);
        SceneAssets assets(referenceSceneAssetPaths());
        Texture observer;
        std::array<Material, 3> materialObservers;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            Texture texture;
            {
                // 第 0 级：BBWW；第 1 级：BW；第 2 级为线性灰色（sRGB 188）。
                std::vector<uint8_t> pixels(4 * 4 * 4);
                for (size_t i = 0; i < 16; ++i)
                    for (int channel = 0; channel < 4; ++channel)
                        pixels[i * 4 + channel] = (i % 4 >= 2) ? 255 : 0;
                Resource::TextureResource source({}, 4, 4, std::move(pixels));
                EXPECT_THROW(renderer.createTexture(source, static_cast<TextureMipMode>(99)), Error::Exception);
                texture = renderer.createTexture(source, TextureMipMode::FullChain);
            }
            std::array<Material, 3> materials;
            const std::array modes{MipFilter::BaseLevel, MipFilter::Nearest, MipFilter::Linear};
            MaterialDesc desc{texture};
            for (size_t i = 0; i < materials.size(); ++i)
            {
                desc.sampler = {TextureFilter::Nearest, TextureFilter::Nearest, modes[i]};
                materials[i] = renderer.createMaterial(desc);
            }
            observer = texture;
            materialObservers = materials;
            desc = {};
            releaseCpuAssets(assets);
            for (float lod : {0.0f, 1.25f, 1.75f, 10.0f})
            {
                SCOPED_TRACE(lod);
                // 4 个纹素对应 128 个像素可得到所需的隐式 LOD。中心 u=0.625：
                // 基础级和第 1 级为白色，第 2 级为灰色。Mip Linear 在 RGB 线性空间中混合。
                const float step = std::exp2(lod) / 4.0f;
                const float u0 = 0.625f - 64.5f * step;
                const float u1 = u0 + 128.0f * step;
                const std::array<MeshVertex, 4> vertices{{{{-1, -1, 0.5f}, {u0, 0.5f}},
                                                          {{0, -1, 0.5f}, {u1, 0.5f}},
                                                          {{0, 1, 0.5f}, {u1, 0.5f}},
                                                          {{-1, 1, 0.5f}, {u0, 0.5f}}}};
                const std::array<uint32_t, 6> indices{0, 1, 2, 2, 3, 0};
                const auto mesh = renderer.createMesh(vertices, indices);
                for (size_t i = 1; i < materials.size(); ++i)
                {
                    SCOPED_TRACE(i);
                    const std::array draws{
                        DrawItem{mesh, materials[0], glm::mat4(1)},
                        DrawItem{mesh, materials[i], glm::translate(glm::mat4(1), glm::vec3(1, 0, 0))}};
                    const FrameInput input{{256, 256}, {glm::mat4(1), glm::mat4(1)}, draws};
                    std::vector<uint8_t> pixels;
                    Detail::RendererReviewAccess::withFrame(
                        renderer, input,
                        [&](const auto& device, const auto& pipeline, auto frameSet, auto nativeDraws) {
                            pixels = TextureGpuReview::captureDraws(device, pipeline, frameSet, nativeDraws,
                                                                    {0, 0, 256, 256, 0, 1});
                        });
                    const float weight = std::clamp(lod - 1.0f, 0.0f, 1.0f);
                    const float linear = i == 1 ? (lod < 1.5f ? 1.0f : 0.5f) : 1.0f - 0.5f * weight;
                    const float encoded = 255.0f * (1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f);
                    for (int channel = 0; channel < 3; ++channel)
                    {
                        EXPECT_NEAR(pixels[(128 * 384 + 64) * 4 + channel], 255, 1);
                        EXPECT_NEAR(pixels[(128 * 384 + 192) * 4 + channel], encoded, 2);
                    }
                    EXPECT_EQ(pixels[(128 * 384 + 192) * 4 + 3], 255);
                    WindowCapture::save({384, 256, pixels}, "review/mip-lod-" + std::to_string(lod) + "-mode-" +
                                                                std::to_string(i) + ".ppm");
                    if (lod == 1.75f && i == 1)
                        glfwSetWindowSize(static_cast<GLFWwindow*>(window->getNativeHandle()), 800, 600);
                    window->pollEvents();
                    const auto extent = renderer.beginUiFrame();
                    ASSERT_TRUE(extent);
                    renderer.imgui().endFrame();
                    renderer.drawFrame({*extent, input.camera, draws}, {0.2f, 0.24f, 0.28f, 1});
                }
            }
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 1u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline:"), 1u);
            texture = {};
            materials = {};
            EXPECT_TRUE(observer.valid());
            for (const auto& material : materialObservers)
                EXPECT_TRUE(material.valid());
        }
        EXPECT_FALSE(observer.valid());
        for (const auto& material : materialObservers)
            EXPECT_FALSE(material.valid());
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, MaterialsCopyParametersShareTexturesAndReusePipeline)
    {
        Log::Registry::get("render").setLevel(Log::Level::DEBUG);
        Log::Registry::get("render.vulkan").setLevel(Log::Level::DEBUG);
        auto window = createWindow(true, 640, 480);
        SceneAssets assets(referenceSceneAssetPaths());
        Texture textureObserver;
        std::array<Material, 3> observers;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            auto texture = renderer.createTexture(*assets.texture);
            textureObserver = texture;
            Resource::TextureResource whiteCpu({}, 1, 1, {255, 255, 255, 0});
            auto otherTexture = renderer.createTexture(whiteCpu);
            MaterialDesc desc{texture};
            EXPECT_EQ(desc.color, (std::array{1.0f, 1.0f, 1.0f}));
            auto white = renderer.createMaterial(desc);
            desc.color = {0.25f, 0.5f, 1.0f};
            auto tinted = renderer.createMaterial(desc);
            observers[0] = white;
            observers[1] = tinted;
            EXPECT_FALSE(Material{}.valid());
            for (int channel = 0; channel < 3; ++channel)
            {
                for (const float value :
                     {-0.1f, 1.1f, std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN()})
                {
                    auto invalid = desc;
                    invalid.color[channel] = value;
                    EXPECT_THROW(renderer.createMaterial(invalid), Error::Exception);
                }
            }
            const auto data = Demo::makePlaneMesh();
            const auto mesh = renderer.createMesh(data.vertices, data.indices);
            releaseCpuAssets(assets);
            auto present = [&](const Material& selected) {
                const std::array draws{DrawItem{mesh, selected, glm::mat4(1.0f)}};
                for (int frame = 0; frame < 6; ++frame)
                {
                    submitControlledScene(*window, renderer, draws);
                    glfwWaitEventsTimeout(0.025);
                }
                return WindowCapture::capture(*window);
            };
            const auto original = present(white);
            // 使用另一张纹理的第三个材质不会替换着色器、相机描述符集或 Pipeline。
            observers[2] = renderer.createMaterial({otherTexture, {0.0f, 1.0f, 0.0f}});
            desc.texture = {};
            desc.color = {0.0f, 0.0f, 0.0f};
            texture = {};
            otherTexture = {};
            white = {};
            tinted = {};
            const auto unchanged = present(observers[0]);
            size_t changed = 0;
            for (int y = 8; y < original.height - 8; ++y)
                for (int x = 8; x < original.width - 8; ++x)
                {
                    const size_t offset = (static_cast<size_t>(y) * original.width + x) * 4;
                    for (int channel = 0; channel < 3; ++channel)
                        changed += original.rgba[offset + channel] != unchanged.rgba[offset + channel];
                }
            EXPECT_EQ(changed, 0u);
            const auto image = present(observers[1]);
            const auto bounds = WindowCapture::bounds(image);
            ASSERT_GT(bounds[2] - bounds[0], 100);
            // desc 清空后，引用中的白色纹素仍能显示已复制的非零 RGB 色调。
            const int x = bounds[0] + 7 * (bounds[2] - bounds[0]) / 8;
            const int y = bounds[1] + (bounds[3] - bounds[1]) / 2;
            const auto offset = (static_cast<size_t>(y) * image.width + x) * 4;
            EXPECT_GT(image.rgba[offset], 50);
            EXPECT_GT(int(image.rgba[offset + 1]), int(image.rgba[offset]) + 20);
            EXPECT_GT(int(image.rgba[offset + 2]), int(image.rgba[offset + 1]) + 20);
            WindowCapture::save(image, "review/material-immutable-tint.ppm");
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline"), 1u);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created material:"), 3u);
            EXPECT_TRUE(textureObserver.valid());
            for (const auto& material : observers)
                EXPECT_TRUE(material.valid());
        }
        EXPECT_FALSE(textureObserver.valid());
        for (const auto& material : observers)
            EXPECT_FALSE(material.valid());
        expectReleased(referenceSceneAssetPaths());
        expectCleanValidation();
    }

    static_assert(!std::is_copy_constructible_v<GpuResources>);
    static_assert(std::is_nothrow_move_constructible_v<GpuResources>);

    TEST_F(VulkanReviewTest, ScopedResourcesPreserveDependenciesAndRollbackWithoutTouchingDirectAssets)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto geometry = Demo::makeCubeMesh();
            const auto directMesh = renderer.createMesh(geometry.vertices, geometry.indices);
            const auto directTexture = renderer.createTexture(*assets.texture);
            const auto directMaterial = renderer.createMaterial({directTexture});
            const auto expectCounts = [&](size_t meshes, size_t textures, size_t materials) {
                const auto counts = Detail::RendererReviewAccess::resourceCounts(renderer);
                EXPECT_EQ(counts.meshes, meshes);
                EXPECT_EQ(counts.textures, textures);
                EXPECT_EQ(counts.materials, materials);
            };
            expectCounts(1, 1, 1);
            GpuResources invalid;
            EXPECT_THROW(renderer.createMesh(invalid, geometry.vertices, geometry.indices), Error::Exception);
            EXPECT_THROW(renderer.createTexture(invalid, *assets.texture), Error::Exception);
            EXPECT_THROW(renderer.createMaterial(invalid, {directTexture}), Error::Exception);
            {
                auto textures = renderer.createResources();
                auto bindings = renderer.createResources();
                const auto texture = renderer.createTexture(textures, *assets.texture);
                const auto material = renderer.createMaterial(bindings, {texture});
                const auto mesh = renderer.createMesh(bindings, geometry.vertices, geometry.indices);
                textures.reset();
                EXPECT_TRUE(texture.valid());  // 材质独立持有所用纹理。
                expectCounts(2, 2, 2);
                auto moved = std::move(bindings);
                EXPECT_FALSE(bindings.valid());
                auto assigned = renderer.createResources();
                const auto discarded = renderer.createMesh(assigned, geometry.vertices, geometry.indices);
                assigned = std::move(moved);
                EXPECT_FALSE(discarded.valid());
                EXPECT_FALSE(moved.valid());
                EXPECT_TRUE(mesh.valid());
                EXPECT_TRUE(material.valid());
                const std::array unsubmitted{DrawItem{mesh, material, glm::mat4(1)}};
                assigned.reset();
                EXPECT_FALSE(mesh.valid());
                EXPECT_FALSE(material.valid());
                EXPECT_FALSE(texture.valid());
                EXPECT_THROW(renderer.drawFrame(controlledFrame({640, 360}, unsubmitted), {0, 0, 0, 1}),
                             Error::Exception);
            }
            expectCounts(1, 1, 1);
            // 在不同创建进度中断；不靠生产故障注入模拟 Vulkan 分配失败。
            for (int stage = 0; stage != 3; ++stage)
            {
                Mesh interruptedMesh;
                Texture interruptedTexture;
                Material interruptedMaterial;
                EXPECT_THROW(
                    {
                        auto attempt = renderer.createResources();
                        interruptedMesh = renderer.createMesh(attempt, geometry.vertices, geometry.indices);
                        if (stage >= 1)
                            interruptedTexture = renderer.createTexture(attempt, *assets.texture);
                        if (stage >= 2)
                            interruptedMaterial = renderer.createMaterial(attempt, {interruptedTexture});
                        throw std::runtime_error("controlled preparation interruption");
                    },
                    std::runtime_error);
                EXPECT_FALSE(interruptedMesh.valid());
                EXPECT_FALSE(interruptedTexture.valid());
                EXPECT_FALSE(interruptedMaterial.valid());
                expectCounts(1, 1, 1);
            }
            releaseCpuAssets(assets);
            EXPECT_TRUE(directTexture.valid());
            EXPECT_GT(drawUiFrame(*window, renderer, directMesh, directMaterial), 0);
        }
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, ScopedMainAndShadowResourcesStayUntilTheirFrameFencesComplete)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            auto depth = Resource::ResourceManager::instance().hold<Resource::ShaderResource>(
                Resource::ResourceType::SHADER, "shaders/shadow.vert.spv");
            ASSERT_TRUE(depth);
            renderer.prepareShadowResources(*depth.get<Resource::ShaderResource>());
            depth.reset();
            const auto geometry = Demo::makeCubeMesh();
            for (const bool shadow : {false, true})
            {
                auto resources = renderer.createResources();
                const auto texture = renderer.createTexture(resources, *assets.texture);
                const auto mesh = renderer.createMesh(resources, geometry.vertices, geometry.indices);
                const auto material = renderer.createMaterial(resources, {texture, {0, 1, 0}});
                Mesh caster;
                if (shadow)
                    caster = renderer.createMesh(resources, geometry.vertices, geometry.indices);
                const std::array draws{DrawItem{mesh, material, glm::mat4(1)}};
                const std::array casters{DrawItem{caster, {}, glm::mat4(1)}};
                SceneOutput output;
                for (int slot = 0; slot != 2; ++slot)
                {
                    const auto extent = renderer.beginUiFrame();
                    ASSERT_TRUE(extent);
                    renderer.imgui().endFrame();
                    if (shadow)
                    {
                        output = renderer.prepareSceneTarget({384, 256});
                        auto frame = controlledFrame(output.extent(), draws);
                        ShadowSettings settings;
                        settings.enabled = true;
                        frame.shadow = makeShadowFrame(settings, frame.lighting.direction);
                        frame.shadowDraws = casters;
                        renderer.drawSceneTarget(output, frame, {0.2f, 0.24f, 0.28f, 1});
                    }
                    else
                        renderer.drawFrame(controlledFrame(*extent, draws), {0.2f, 0.24f, 0.28f, 1});
                }
                resources.reset();
                EXPECT_TRUE(mesh.valid());
                EXPECT_TRUE(texture.valid());
                EXPECT_TRUE(material.valid());
                auto counts = Detail::RendererReviewAccess::resourceCounts(renderer);
                EXPECT_EQ(counts.meshes, shadow ? 2u : 1u);
                EXPECT_EQ(counts.textures, 1u);
                EXPECT_EQ(counts.materials, 1u);
                // 只复用一个槽不会撤销另一槽的保护；帧数据本身仅保存弱引用。
                renderer.prepareSceneTarget({384, 256});
                EXPECT_TRUE(mesh.valid());
                if (shadow)
                {
                    EXPECT_TRUE(caster.valid());
                    Detail::RendererReviewAccess::withSceneOutput(
                        renderer, output, [&](const auto& device, const auto& image) {
                            const auto pixels = TextureGpuReview::captureMip(device, image, 0, {384, 256});
                            ASSERT_EQ(pixels.size(), 384u * 256u * 4u);
                            EXPECT_LT(pixels[(128u * 384u + 192u) * 4u], 5u);
                        });
                }
                else
                    Detail::RendererReviewAccess::waitForFrames(renderer);
                EXPECT_FALSE(mesh.valid());
                EXPECT_FALSE(material.valid());
                EXPECT_FALSE(texture.valid());
                EXPECT_FALSE(caster.valid());
                counts = Detail::RendererReviewAccess::resourceCounts(renderer);
                EXPECT_EQ(counts.meshes, 0u);
                EXPECT_EQ(counts.textures, 0u);
                EXPECT_EQ(counts.materials, 0u);
                EXPECT_THROW(renderer.createMaterial({texture}), Error::Exception);
            }
            releaseCpuAssets(assets);
        }
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, SharedScopedOwnerSurvivesCacheRemovalAndBecomesSafeEmptyAfterRendererClose)
    {
        auto window = createWindow();
        SceneAssets assets;
        std::shared_ptr<GpuResources> external;
        Mesh mesh;
        Texture texture;
        Material material;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            auto cache = std::make_shared<GpuResources>(renderer.createResources());
            external = cache;
            const auto geometry = Demo::makeCubeMesh();
            mesh = renderer.createMesh(*cache, geometry.vertices, geometry.indices);
            texture = renderer.createTexture(*cache, *assets.texture);
            material = renderer.createMaterial(*cache, {texture});
            cache.reset();
            EXPECT_TRUE(external->valid());
            EXPECT_TRUE(mesh.valid());
            EXPECT_GT(drawUiFrame(*window, renderer, mesh, material), 0);
            Renderer moved(std::move(renderer));
            EXPECT_TRUE(external->valid());
            EXPECT_GT(drawUiFrame(*window, moved, mesh, material), 0);
            releaseCpuAssets(assets);
        }
        EXPECT_FALSE(external->valid());
        EXPECT_FALSE(mesh.valid());
        EXPECT_FALSE(texture.valid());
        EXPECT_FALSE(material.valid());
        external.reset();  // 晚析构的组不调用已销毁 Renderer 或设备。
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, SubmissionRejectsEmptyExpiredResourcesAndMovedFromRenderer)
    {
        auto window = createWindow();
        SceneAssets assets;
        Material expired;
        Mesh expiredMesh;
        {
            Renderer previous(*window, RendererValidationMode::Required);
            previous.prepareSceneResources(*assets.vertex, *assets.fragment);
            expired = previous.createMaterial({previous.createTexture(*assets.texture)});
            expiredMesh = createCubeMesh(previous);
        }
        EXPECT_FALSE(expired.valid());
        Material observer;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            MaterialDesc desc{renderer.createTexture(*assets.texture)};
            observer = renderer.createMaterial(desc);
            Renderer moved(std::move(renderer));
            const auto output = moved.prepareSceneTarget({320, 240});
            const auto expectMovedFrom = [](const auto& operation) {
                try
                {
                    operation();
                    FAIL() << "Accepted moved-from Renderer";
                }
                catch (const Error::Exception& error)
                {
                    EXPECT_EQ(error.code(), Error::Code::RENDER);
                }
            };
            expectMovedFrom([&] { renderer.prepareSceneTarget({320, 240}); });
            expectMovedFrom([&] { renderer.drawSceneTarget(output, {}, {0, 0, 0, 1}); });
            expectMovedFrom([&] { renderer.sceneImage(output); });
            expectMovedFrom([&] { renderer.createResources(); });
            EXPECT_NE(moved.sceneImage(output), 0u);
            moved.drawSceneTarget(output, controlledFrame(output.extent(), {}), {0, 0, 0, 1});
            EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            EXPECT_THROW(renderer.prepareSceneResources(*assets.vertex, *assets.fragment), Error::Exception);
            EXPECT_TRUE(moved.createMaterial(desc).valid());
            const auto mesh = createCubeMesh(moved);
            releaseCpuAssets(assets);
            const auto extent = moved.beginUiFrame();
            ASSERT_TRUE(extent);
            moved.imgui().endFrame();
            int recorded = 0;
            for (const auto& invalid : {DrawItem{mesh, {}, glm::mat4(1.0f)}, DrawItem{mesh, expired, glm::mat4(1.0f)},
                                        DrawItem{expiredMesh, observer, glm::mat4(1.0f)}})
            {
                // 第一个绘制项有效；后续项目失败时，必须在录制前拒绝整个列表。
                const std::array draws{DrawItem{mesh, observer, glm::mat4(1.0f)}, invalid};
                EXPECT_THROW(moved.drawFrame(controlledFrame(*extent, draws), {0.2f, 0.24f, 0.28f, 1.0f},
                                             [&](void*) { ++recorded; }),
                             Error::Exception);
            }
            EXPECT_EQ(recorded, 0);
            const std::array valid{DrawItem{mesh, observer, glm::mat4(1.0f)}};
            EXPECT_THROW(renderer.drawFrame(controlledFrame(*extent, valid), {0.2f, 0.24f, 0.28f, 1.0f}),
                         Error::Exception);
            moved.drawFrame(controlledFrame(*extent, valid), {0.2f, 0.24f, 0.28f, 1.0f});
            EXPECT_GT(drawUiFrame(*window, moved, mesh, observer), 0);
        }
        EXPECT_FALSE(observer.valid());
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, MaterialTintMatchesLinearRgbAndOpaqueAlphaOnGpu)
    {
        auto window = createWindow(false, 384, 256);
        {
            VulkanInstance instance(requiredExtensions(), ValidationMode::Required);
            GlfwSurface surface(instance, *window);
            VulkanDevice device(instance, surface);
            VulkanSwapchain swapchain(device, surface, 384, 256);
            SceneAssets assets(referenceSceneAssetPaths());
            const auto pixels = TextureGpuReview::capture(instance.handle(), device, *window, assets,
                                                          swapchain.format(), {0.5f, 0.5f, 0.5f});
            const size_t white = (112 * 384 + 240) * 4;
            const size_t gray = (112 * 384 + 176) * 4;
            for (int channel = 0; channel < 3; ++channel)
            {
                EXPECT_NEAR(pixels[white + channel], 188, 1);
                EXPECT_NEAR(pixels[gray + channel], 92, 1);
                EXPECT_NEAR(pixels[(64 * 384 + 320) * 4 + channel], 128, 1);   // UI 不应受颜色调制。
                EXPECT_NEAR(pixels[(160 * 384 + 320) * 4 + channel], 128, 1);  // 清屏颜色不应受颜色调制。
            }
            for (int y = 0; y < 3; ++y)
                for (int x = 0; x < 4; ++x)
                    EXPECT_EQ(pixels[((16 + y * 64 + 32) * 384 + 16 + x * 64 + 32) * 4 + 3], 255);
            WindowCapture::save({384, 256, pixels}, "review/material-half-linear-gpu.ppm");
            expectReleased(referenceSceneAssetPaths());
            std::cout << "GPU_MATERIAL: FORMAT=" << int(swapchain.format())
                      << " COLOR_SPACE=" << int(swapchain.colorSpace()) << " LINEAR_TINT=0.5"
                      << " WHITE=" << int(pixels[white]) << " GRAY=" << int(pixels[gray])
                      << " ALPHA=" << int(pixels[white + 3]) << " RGB_TOLERANCE=1\n";
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, MaterialBindingsSurviveControlledPipelineFormatRebuild)
    {
        Log::Registry::get("render.vulkan").setLevel(Log::Level::DEBUG);
        auto window = createWindow(false, 384, 256);
        {
            VulkanInstance instance(requiredExtensions(), ValidationMode::Required);
            GlfwSurface surface(instance, *window);
            VulkanDevice device(instance, surface);
            VulkanSwapchain swapchain(device, surface, 384, 256);
            const auto target =
                swapchain.format() == VK_FORMAT_B8G8R8A8_UNORM ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
            SceneAssets assets(referenceSceneAssetPaths());
            const auto pixels = TextureGpuReview::capture(instance.handle(), device, *window, assets, target,
                                                          {0.5f, 0.5f, 0.5f}, swapchain.format());
            const auto white = (112 * 384 + 240) * 4;
            const auto gray = (112 * 384 + 176) * 4;
            for (int channel = 0; channel < 3; ++channel)
            {
                EXPECT_NEAR(pixels[white + channel], 188, 1);
                EXPECT_NEAR(pixels[gray + channel], 92, 1);
                EXPECT_NEAR(pixels[(64 * 384 + 320) * 4 + channel], 128, 1);
            }
            EXPECT_EQ(pixels[white + 3], 255);
            EXPECT_EQ(pixels[((16 + 2 * 64 + 32) * 384 + 16 + 2 * 64 + 32) * 4 + 3], 255);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 1u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline:"), 2u);
            expectReleased(referenceSceneAssetPaths());
            WindowCapture::save({384, 256, pixels}, "review/material-format-rebuild-gpu.ppm");
            std::cout << "CONTROLLED_PIPELINE_FORMAT: ACTUAL_SWAPCHAIN=" << int(swapchain.format())
                      << " OFFSCREEN_REBUILD=" << int(swapchain.format()) << "->" << int(target)
                      << " CPU_RELEASED_BEFORE_REBUILD=1 TEXTURE_UPLOADS=1 PIPELINES=2"
                      << " WHITE=" << int(pixels[white]) << " ALPHA=" << int(pixels[white + 3]) << '\n';
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, PerItemMaterialsShareResourcesAcrossFramesResizeAndCpuListRelease)
    {
        Log::Registry::get("render").setLevel(Log::Level::DEBUG);
        Log::Registry::get("render.vulkan").setLevel(Log::Level::DEBUG);
        auto native = createWindow(true, 640, 480);
        DeferredFramebufferWindow window(*native);
        window.drawable = true;
        FileAudit referenceAudit;
        FileAudit directionAudit;
        referenceAudit.paths = referenceSceneAssetPaths();
        directionAudit.paths.texture = "textures/direction_rgba.png";
        std::array<Material, 3> observers;
        std::array<Texture, 2> textureObservers;
        Mesh meshObserver;
        {
            Renderer renderer(window, RendererValidationMode::Required);
            Material white;
            Material half;
            Material direction;
            {
                SceneAssets assets(referenceAudit.paths, std::make_unique<AuditedFileIO>(referenceAudit));
                renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
                auto texture = renderer.createTexture(*assets.texture);
                textureObservers[0] = texture;
                MaterialDesc desc{texture};
                white = renderer.createMaterial(desc);
                desc.color = {0.5f, 0.5f, 0.5f};
                half = renderer.createMaterial(desc);
                desc = {};
                releaseCpuAssets(assets);
            }
            {
                SceneAssets assets(directionAudit.paths, std::make_unique<AuditedFileIO>(directionAudit));
                auto texture = renderer.createTexture(*assets.texture);
                textureObservers[1] = texture;
                direction = renderer.createMaterial({texture});
                releaseCpuAssets(assets);
            }
            observers = {white, half, direction};
            auto cube = createCubeMesh(renderer);
            auto plane = [&] {
                const auto data = Demo::makePlaneMesh();
                return renderer.createMesh(data.vertices, data.indices);
            }();
            meshObserver = plane;
            const auto model = [](float x, float y) {
                return glm::translate(glm::mat4(1.0f), glm::vec3(x, y, 0.0f)) *
                       glm::scale(glm::mat4(1.0f), glm::vec3(0.65f));
            };
            FrameExtent lastExtent{};
            for (int frame = 0; frame < 8; ++frame)
            {
                if (frame == 4)
                    glfwSetWindowSize(static_cast<GLFWwindow*>(window.getNativeHandle()), 800, 600);
                if (frame == 5)
                {
                    window.drawable = false;
                    const std::array stale{DrawItem{plane, half, model(0, 0)}};
                    renderer.drawFrame(controlledFrame(lastExtent, stale), {0.2f, 0.24f, 0.28f, 1.0f});
                    window.drawable = true;
                }
                const bool switched = frame % 2 != 0;
                std::vector<DrawItem> draws = {{plane, switched ? half : white, model(-0.65f, 0.45f)},
                                               {cube, white, model(-0.65f, -0.45f)},
                                               {plane, switched ? white : half, model(0.65f, 0.45f)},
                                               {plane, direction, model(0.65f, -0.45f)}};
                if (switched)
                    std::reverse(draws.begin(), draws.end());
                lastExtent = submitControlledScene(window, renderer, draws, true);
                for (auto& draw : draws)
                    draw = {};
                draws.clear();
                draws.shrink_to_fit();
                if (frame == 7)
                {
                    white = half = direction = {};
                    cube = plane = {};
                }
                glfwWaitEventsTimeout(0.06);
                const auto image = WindowCapture::capture(window);
                const auto sample = [&](float x, float y, int channel) {
                    const int px = image.width / 2 + int(x * image.height / 2);
                    const int py = image.height / 2 - int(y * image.height / 2);
                    return image.rgba[(static_cast<size_t>(py) * image.width + px) * 4 + channel];
                };
                for (int channel = 0; channel < 3; ++channel)
                {
                    // 显示效果检查的容差大于独立离屏数值测试的容差。
                    EXPECT_NEAR(sample(-0.65f + 0.24375f, 0.45f, channel), switched ? 188 : 255, 12);
                    EXPECT_NEAR(sample(0.65f + 0.24375f, 0.45f, channel), switched ? 255 : 188, 12);
                    EXPECT_GT(sample(-0.65f + 0.24375f, -0.45f, channel), 220);
                }
                EXPECT_GT(sample(0.65f + 0.21667f, -0.45f + 0.08125f, 0), 220);
                EXPECT_LT(sample(0.65f + 0.21667f, -0.45f + 0.08125f, 1), 30);
                EXPECT_LT(sample(0.65f + 0.21667f, -0.45f + 0.08125f, 2), 30);
                EXPECT_EQ(image.width, frame < 4 ? 640 : 800);
                if (frame == 0 || frame == 7)
                    WindowCapture::save(image, frame == 0 ? "review/material-combinations.ppm"
                                                          : "review/material-switched-resized.ppm");
            }
            for (const auto& material : observers)
                EXPECT_TRUE(material.valid());
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Uploaded mesh:"), 2u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created material:"), 3u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline:"), 1u);
            EXPECT_EQ(referenceAudit.textureOpens, 1u);
            EXPECT_EQ(directionAudit.textureOpens, 1u);
            EXPECT_TRUE(referenceAudit.cpuReleasedBeforeDestroy);
            EXPECT_TRUE(directionAudit.cpuReleasedBeforeDestroy);
            std::cout << "PER_ITEM_MATERIAL: FRAMES=8 MATERIALS=3 TEXTURES=2 MESHES=2 PIPELINES=1 "
                         "SHARED_MESH=1 SHARED_MATERIAL=1 SHARED_TEXTURE=1 REVERSED_ORDER=1 "
                         "SWITCHED_REFERENCES=1 CPU_LIST_RELEASED=1 RESIZE=800x600 SKIPPED_LIST_NOT_RETAINED=1\n";
        }
        EXPECT_FALSE(meshObserver.valid());
        for (const auto& material : observers)
            EXPECT_FALSE(material.valid());
        for (const auto& texture : textureObservers)
            EXPECT_FALSE(texture.valid());
        expectReleased(referenceAudit.paths);
        expectReleased(directionAudit.paths);
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, InitializationFailureLeavesNextRendererUsable)
    {
        auto native = createWindow();
        DeferredFramebufferWindow window(*native);
        window.throwOnSize = true;
        EXPECT_THROW((Renderer(window, RendererValidationMode::Required)), Error::Exception);
        {
            Renderer recovered(*native, RendererValidationMode::Required);
            submitControlledScene(*native, recovered, {}, true);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, TextureSamplesMatchReferenceDirectionAndUiOnGpu)
    {
        auto window = createWindow(false, 384, 256);
        for (const bool direction : {false, true})
        {
            SCOPED_TRACE(direction ? "direction" : "reference");
            auto paths = referenceSceneAssetPaths();
            if (direction)
                paths.texture = "textures/direction_rgba.png";
            SceneAssets assets(paths);
            const int columns = direction ? 3 : 4;
            const int rows = direction ? 4 : 3;
            ASSERT_EQ(assets.texture->width(), columns);
            ASSERT_EQ(assets.texture->height(), rows);
            const std::array<std::array<int, 3>, 12> reference = {{{255, 0, 0},
                                                                   {0, 255, 0},
                                                                   {0, 0, 255},
                                                                   {255, 255, 0},
                                                                   {0, 255, 255},
                                                                   {255, 0, 255},
                                                                   {128, 128, 128},
                                                                   {255, 255, 255},
                                                                   {255, 128, 0},
                                                                   {128, 0, 255},
                                                                   {17, 34, 51},
                                                                   {0, 0, 0}}};
            const std::vector<uint8_t> directional = {255, 255, 255, 255, 255, 128, 0,   255, 0,   0,   0,   255,
                                                      0,   0,   255, 255, 128, 128, 128, 255, 255, 0,   0,   255,
                                                      0,   255, 0,   255, 255, 0,   255, 128, 0,   255, 255, 255,
                                                      17,  34,  51,  0,   255, 255, 0,   255, 128, 0,   255, 255};
            if (direction)
                EXPECT_EQ(assets.texture->pixels(), directional);
            VulkanInstance instance(requiredExtensions(), ValidationMode::Required);
            GlfwSurface surface(instance, *window);
            VulkanDevice device(instance, surface);
            VulkanSwapchain swapchain(device, surface, 384, 256);
            EXPECT_EQ(swapchain.colorSpace(), VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
            const auto pixels =
                TextureGpuReview::capture(instance.handle(), device, *window, assets, swapchain.format());
            for (int row = 0; row < rows; ++row)
                for (int col = 0; col < columns; ++col)
                {
                    const size_t source = row * columns + col;
                    const int x = 16 + (2 * col + 1) * 256 / (2 * columns);
                    const int y = 16 + (2 * row + 1) * 192 / (2 * rows);
                    const size_t offset = (y * 384 + x) * 4;
                    for (int channel = 0; channel < 3; ++channel)
                        EXPECT_NEAR(pixels[offset + channel],
                                    direction ? directional[source * 4 + channel] : reference[source][channel], 1);
                    EXPECT_EQ(pixels[offset + 3], 255);
                }
            for (const auto position : {std::array{320, 64}, std::array{320, 160}})
                for (int channel = 0; channel < 3; ++channel)
                    EXPECT_NEAR(pixels[(position[1] * 384 + position[0]) * 4 + channel], 128, 1);
            const auto marker = (34 * 384 + 290) * 4;
            EXPECT_EQ(pixels[marker], 255);
            EXPECT_EQ(pixels[marker + 1], 0);
            EXPECT_EQ(pixels[marker + 2], 255);
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(pixels[(200 * 384 + 320) * 4 + channel], 64, 1);
            expectReleased(paths);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, TextureUploadRejectsDeviceOversizeAndMismatchedPayload)
    {
        auto window = createWindow();
        {
            VulkanInstance instance(requiredExtensions(), ValidationMode::Required);
            GlfwSurface surface(instance, *window);
            VulkanDevice device(instance, surface);
            const std::array<uint8_t, 4> pixel{128, 128, 128, 255};
            EXPECT_THROW(VulkanImage::uploadRgba8(device, {device.properties().limits.maxImageDimension2D + 1, 1},
                                                  VK_FORMAT_R8G8B8A8_SRGB, pixel, VK_IMAGE_USAGE_SAMPLED_BIT),
                         Error::Exception);
            EXPECT_THROW(
                VulkanImage::uploadRgba8(device, {2, 1}, VK_FORMAT_R8G8B8A8_SRGB, pixel, VK_IMAGE_USAGE_SAMPLED_BIT),
                Error::Exception);
            auto image =
                VulkanImage::uploadRgba8(device, {1, 1}, VK_FORMAT_R8G8B8A8_SRGB, pixel, VK_IMAGE_USAGE_SAMPLED_BIT);
            EXPECT_EQ(image->format(), VK_FORMAT_R8G8B8A8_SRGB);
            EXPECT_NE(image->view(), VK_NULL_HANDLE);
        }
        expectCleanValidation();
    }
    TEST_F(VulkanReviewTest, LambertMatchesAnglesLinearColorsAndImmutableUnlitSelection)
    {
        Log::Registry::get("render").setLevel(Log::Level::DEBUG);
        Log::Registry::get("render.vulkan").setLevel(Log::Level::DEBUG);
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const Resource::TextureResource cpu({}, 1, 1, {128, 64, 192, 0});
            const auto texture = renderer.createTexture(cpu);
            auto data = Demo::makePlaneMesh();
            for (auto& vertex : data.vertices)
                vertex.normal[2] = 1;
            const auto mesh = renderer.createMesh(data.vertices, data.indices);
            const std::array<float, 3> tint{0.5f, 0.75f, 0.25f};
            MaterialDesc desc{texture, tint};
            EXPECT_EQ(desc.shading, ShadingMode::Unlit);
            const auto unlit = renderer.createMaterial(desc);
            desc.shading = static_cast<ShadingMode>(99);
            EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created material:"), 1u);
            desc.shading = ShadingMode::Lambert;
            const auto lit = renderer.createMaterial(desc);
            desc.shading = ShadingMode::Unlit;
            desc.color = {0, 0, 0};
            desc.texture = {};
            releaseCpuAssets(assets);
            const auto base =
                linearFromSrgb({128 / 255.0f, 64 / 255.0f, 192 / 255.0f}) * glm::vec3(tint[0], tint[1], tint[2]);
            std::array draws{DrawItem{mesh, lit, glm::mat4(1)}};
            auto frame = controlledFrame({384, 256}, draws);
            frame.lighting = {{0, 0, -1}, {0.3f, 0.7f, 0.5f}, 1.2f, 0.15f};
            for (const float cosine : {1.0f, 0.5f, 0.0f, -1.0f})
            {
                SCOPED_TRACE(cosine);
                frame.lighting.direction = {-std::sqrt(1 - cosine * cosine), 0, -cosine};
                const auto pixels = TextureGpuReview::captureFrame(renderer, frame);
                expectLinearPixel(pixels,
                                  base * (glm::vec3(frame.lighting.ambient) +
                                          frame.lighting.color * frame.lighting.intensity * std::max(cosine, 0.0f)));
                draws[0].material = unlit;
                expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame), base);
                draws[0].material = lit;
            }
            frame.lighting.intensity = 0;
            frame.lighting.ambient = 0.25f;
            expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame), base * 0.25f);
            frame.lighting.ambient = 0;
            expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame), glm::vec3(0));
            frame.lighting = {{0, 0, -1}, {1, 1, 1}, 1000, 0};
            expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame), glm::vec3(1));
            frame.lighting = {{0, 0, -1}, {0.3f, 0.7f, 0.5f}, 1.2f, 0.15f};
            const auto expected = base * (glm::vec3(0.15f) + frame.lighting.color * 1.2f);
            for (const glm::vec3 eye : {glm::vec3(0, 0, 3), glm::vec3(1.5f, 0.5f, 3)})
            {
                frame.camera.view = glm::lookAtRH(eye, glm::vec3(0), glm::vec3(0, 1, 0));
                expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame), expected);
            }
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline:"), 1u);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, LambertFiniteNormalMagnitudesPreserveLightingUnderNonuniformScale)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const Resource::TextureResource cpu({}, 1, 1, {255, 255, 255, 255});
            const auto material =
                renderer.createMaterial({renderer.createTexture(cpu), {1, 1, 1}, {}, ShadingMode::Lambert});
            releaseCpuAssets(assets);
            for (const float magnitude : {1.0f, 1e20f, 1e38f})
            {
                SCOPED_TRACE(magnitude);
                auto data = Demo::makePlaneMesh();
                for (auto& vertex : data.vertices)
                    vertex.normal[2] = magnitude;
                const auto mesh = renderer.createMesh(data.vertices, data.indices);
                const auto model = glm::scale(glm::mat4(1), glm::vec3(1, 1, 0.001f));
                const std::array draws{DrawItem{mesh, material, model}};
                auto frame = controlledFrame({384, 256}, draws);
                frame.lighting = {{0, 0, -1}, {1, 1, 1}, 1, 0};
                expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame), glm::vec3(1));
            }
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, LambertZeroCancellationAndSingularNormalsUseOnlyAmbient)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const Resource::TextureResource cpu({}, 1, 1, {255, 255, 255, 255});
            const auto material =
                renderer.createMaterial({renderer.createTexture(cpu), {0.5f, 0.25f, 0.75f}, {}, ShadingMode::Lambert});
            releaseCpuAssets(assets);
            auto plane = Demo::makePlaneMesh();
            const auto zero = renderer.createMesh(plane.vertices, plane.indices);
            for (auto& vertex : plane.vertices)
                vertex.normal[2] = 1;
            const auto normal = renderer.createMesh(plane.vertices, plane.indices);
            const auto clip = [](float x, float y) { return std::array<float, 3>{x / 192 - 1, y / 128 - 1, 0.5f}; };
            const auto a = clip(64.5f, 64.5f), b = clip(320.5f, 64.5f), c = clip(64.5f, 192.5f);
            const std::array<MeshVertex, 3> vertices{{{{a[0], a[1], a[2]}, {0, 0}, {0, 0, 1}},
                                                      {{b[0], b[1], b[2]}, {0, 0}, {0, 0, -1}},
                                                      {{c[0], c[1], c[2]}, {0, 0}, {0, 0, -1}}}};
            const std::array<uint32_t, 3> indices{0, 1, 2};
            const auto cancelling = renderer.createMesh(vertices, indices);
            auto projective = glm::mat4(1);
            projective[0][3] = 0.25f;
            std::array draws{DrawItem{zero, material, glm::mat4(1)}};
            FrameInput frame{{384, 256}, {glm::mat4(1), glm::mat4(1)}, draws, {{0, 0, -1}, {1, 1, 1}, 1, 0.2f}};
            const auto ambient = glm::vec3(0.5f, 0.25f, 0.75f) * 0.2f;
            expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame), ambient);
            for (const auto& model : {glm::scale(glm::mat4(1), glm::vec3(1, 1, 0)), projective})
            {
                draws[0] = {normal, material, model};
                expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame), ambient);
            }
            draws[0] = {cancelling, material, glm::mat4(1)};
            for (const float sign : {-1.0f, 1.0f})
            {
                frame.lighting.direction.z = sign;
                expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame), ambient, 128, 96);
            }
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, LambertFrameSlotsShareLightingAndKeepSubmittedValuesAfterRendererMove)
    {
        Log::Registry::get("render").setLevel(Log::Level::DEBUG);
        auto window = createWindow(true, 768, 512);
        SceneAssets assets;
        Mesh meshObserver;
        Material materialObserver;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            {
                const Resource::TextureResource cpu({}, 1, 1, {255, 255, 255, 0});
                auto data = Demo::makePlaneMesh();
                for (auto& vertex : data.vertices)
                    vertex.normal[2] = 1;
                meshObserver = renderer.createMesh(data.vertices, data.indices);
                materialObserver = renderer.createMaterial(
                    {renderer.createTexture(cpu), {0.25f, 0.5f, 0.75f}, {}, ShadingMode::Lambert});
            }
            releaseCpuAssets(assets);
            Renderer moved(std::move(renderer));
            std::array draws{
                DrawItem{meshObserver, materialObserver, glm::translate(glm::mat4(1), glm::vec3(-0.6f, 0, 0))},
                DrawItem{meshObserver, materialObserver, glm::translate(glm::mat4(1), glm::vec3(0.6f, 0, 0))}};
            for (int index = 0; index < 6; ++index)
            {
                window->pollEvents();
                const auto extent = moved.beginUiFrame();
                ASSERT_TRUE(extent);
                moved.imgui().endFrame();
                auto frame = controlledFrame(*extent, draws);
                frame.lighting = index % 2 ? Lighting{{0, 0, 1}, {1, 0.2f, 0.5f}, 1, 0.3f}
                                           : Lighting{{0, 0, -2}, {0.4f, 0.6f, 1}, 0.8f, 0.1f};
                const auto expected =
                    glm::vec3(0.25f, 0.5f, 0.75f) *
                    (glm::vec3(frame.lighting.ambient) + (index % 2 ? glm::vec3(0) : frame.lighting.color * 0.8f));
                moved.drawFrame(frame, {0, 0, 0, 1});
                frame.lighting = {{1, 0, 0}, {0, 0, 0}, 0, 0};
                glfwWaitEventsTimeout(0.06);
                const auto image = WindowCapture::capture(*window);
                const float aspect = float(extent->width) / extent->height;
                for (const float x : {-0.6f, 0.6f})
                {
                    const auto px = static_cast<uint32_t>((x / aspect + 1) * image.width * 0.5f);
                    expectLinearPixel(image.rgba, expected, px, image.height / 2, image.width);
                }
            }
            EXPECT_TRUE(meshObserver.valid());
            EXPECT_TRUE(materialObserver.valid());
            EXPECT_EQ(countOccurrences(messages_.str(), "Uploaded mesh:"), 1u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created material:"), 1u);
        }
        EXPECT_FALSE(meshObserver.valid());
        EXPECT_FALSE(materialObserver.valid());
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, InvalidFrameLightingRejectsBeforeAcquisitionAndReviewThenFramesRecover)
    {
        auto native = createWindow();
        DeferredFramebufferWindow window(*native);
        window.drawable = true;
        SceneAssets assets;
        {
            Renderer renderer(window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto mesh = createCubeMesh(renderer);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            releaseCpuAssets(assets);
            const std::array draws{DrawItem{mesh, material, glm::mat4(1)}};
            auto frame = Demo::makeSceneFrame({640, 360}, draws);
            std::array<Lighting, 7> invalids{};
            invalids[0].direction = glm::vec3(0);
            invalids[1].direction.x = std::numeric_limits<float>::quiet_NaN();
            invalids[2].color.y = std::numeric_limits<float>::infinity();
            invalids[3].color.z = 1.1f;
            invalids[4].intensity = -1;
            invalids[5].ambient = -0.1f;
            invalids[6].intensity = std::numeric_limits<float>::infinity();
            window.throwOnSize = true;
            bool recorded = false;
            const auto reject = [&](const auto& action) {
                try
                {
                    action();
                    FAIL() << "Invalid frame lighting was accepted";
                }
                catch (const Error::Exception& error)
                {
                    EXPECT_EQ(error.code(), Error::Code::RENDER);
                    EXPECT_NE(error.message().find("Frame lighting"), std::string::npos);
                }
            };
            for (const auto& invalid : invalids)
            {
                frame.lighting = invalid;
                reject([&] { renderer.drawFrame(frame, {0, 0, 0, 1}, [&](void*) { recorded = true; }); });
                reject([&] {
                    Detail::RendererReviewAccess::withFrame(
                        renderer, frame, [&](const auto&, const auto&, auto, auto) { recorded = true; });
                });
                EXPECT_FALSE(recorded);
            }
            window.throwOnSize = false;
            for (int index = 0; index < 6; ++index)
            {
                window.pollEvents();
                const auto extent = renderer.beginUiFrame();
                ASSERT_TRUE(extent);
                renderer.imgui().endFrame();
                frame = Demo::makeSceneFrame(*extent, draws);
                frame.lighting = index % 2 ? Lighting{{0, -4, 0}, {0.25f, 1, 0.5f}, 3, 0.8f}
                                           : Lighting{{0, 0, -5}, {1, 0.5f, 0.25f}, 0, 0};
                EXPECT_NO_THROW(renderer.drawFrame(frame, {0.2f, 0.24f, 0.28f, 1}));
            }
            Detail::RendererReviewAccess::withFrame(
                renderer, frame, [&](const auto& device, const auto& pipeline, auto frameSet, auto nativeDraws) {
                    const auto pixels = TextureGpuReview::captureDraws(device, pipeline, frameSet, nativeDraws);
                    EXPECT_EQ(pixels.size(), 384u * 256 * 4);
                    recorded = true;
                });
            EXPECT_TRUE(recorded);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, FinalModelNormalMatricesAndDegenerateUnlitDrawsRemainValid)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const Resource::TextureResource white({}, 1, 1, {255, 255, 255, 255});
            const auto material = renderer.createMaterial({renderer.createTexture(white)});
            releaseCpuAssets(assets);
            auto data = Demo::makePlaneMesh();
            data.vertices[0].normal[0] = 1;
            data.vertices[1].normal[1] = 2;
            data.vertices[2].normal[2] = 3;
            const auto mesh = renderer.createMesh(data.vertices, data.indices);
            const auto rotation = glm::rotate(glm::mat4(1), glm::radians(37.0f), glm::normalize(glm::vec3(1, 2, 3)));
            const auto nonuniform = rotation * glm::scale(glm::mat4(1), glm::vec3(2, 3, 0.5f));
            auto projective = glm::mat4(1);
            projective[0][3] = 0.25f;
            const std::array models = {glm::mat4(1),
                                       rotation,
                                       nonuniform,
                                       rotation * glm::scale(glm::mat4(1), glm::vec3(-2, 3, 0.5f)),
                                       rotation * glm::scale(glm::mat4(1), glm::vec3(0.001f)),
                                       glm::translate(glm::mat4(1), glm::vec3(7, -2, 4)) * nonuniform,
                                       glm::scale(glm::mat4(1), glm::vec3(1, 1, 0)),
                                       projective};
            std::vector<DrawItem> draws;
            for (const auto& model : models)
                draws.push_back({mesh, material, model});
            const FrameInput frame{{384, 256}, {glm::mat4(1), glm::mat4(1)}, draws};
            Detail::RendererReviewAccess::withFrame(
                renderer, frame, [&](const auto& device, const auto& pipeline, auto frameSet, auto nativeDraws) {
                    ASSERT_EQ(nativeDraws.size(), models.size());
                    for (size_t index = 0; index < models.size(); ++index)
                    {
                        SCOPED_TRACE(index);
                        const auto& object = nativeDraws[index].object;
                        const auto reference =
                            index < 6 ? glm::transpose(glm::inverse(glm::dmat3(models[index]))) : glm::dmat3(0);
                        for (int column = 0; column < 4; ++column)
                            EXPECT_EQ(object.model[column], models[index][column]);
                        for (int column = 0; column < 3; ++column)
                        {
                            EXPECT_EQ(object.normalColumns[column].w, 0);
                            for (int row = 0; row < 3; ++row)
                            {
                                EXPECT_TRUE(std::isfinite(object.normalColumns[column][row]));
                                EXPECT_NEAR(object.normalColumns[column][row], reference[column][row],
                                            0.00001 * std::max(1.0, std::abs(reference[column][row])));
                            }
                        }
                    }
                    for (int column = 0; column < 3; ++column)
                        EXPECT_EQ(nativeDraws[2].object.normalColumns[column],
                                  nativeDraws[5].object.normalColumns[column]);
                    // XY 平面在 Z 轴零缩放或透视模型矩阵下仍有面积，Unlit 继续输出白色。
                    for (size_t index : {6u, 7u})
                    {
                        const auto pixels =
                            TextureGpuReview::captureDraws(device, pipeline, frameSet, nativeDraws.subspan(index, 1));
                        ASSERT_EQ(pixels.size(), 384u * 256 * 4);
                        for (int channel = 0; channel < 4; ++channel)
                            EXPECT_EQ(pixels[(128 * 384 + 192) * 4 + channel], 255);
                    }
                });
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, MeshCreationRejectsInvalidDataAndReferencesExpireAfterRendererClose)
    {
        auto window = createWindow();
        SceneAssets assets;
        Mesh observer;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            releaseCpuAssets(assets);
            auto data = Demo::makePlaneMesh();
            EXPECT_THROW(renderer.createMesh({}, data.indices), Error::Exception);
            EXPECT_THROW(renderer.createMesh(data.vertices, {}), Error::Exception);
            for (const auto& invalid :
                 {std::vector<uint32_t>{0}, std::vector<uint32_t>{0, 1}, std::vector<uint32_t>{0, 1, 2, 3},
                  std::vector<uint32_t>{0, 1, 4}, std::vector<uint32_t>{0, 1, std::numeric_limits<uint32_t>::max()}})
                EXPECT_THROW(renderer.createMesh(data.vertices, invalid), Error::Exception);
            for (const auto& vertex : data.vertices)
                for (const float value : vertex.normal)
                    EXPECT_EQ(value, 0.0f);
            for (int axis = 0; axis < 3; ++axis)
                for (const float invalid :
                     {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity()})
                {
                    auto invalidVertices = data.vertices;
                    invalidVertices.back().normal[axis] = invalid;
                    try
                    {
                        renderer.createMesh(invalidVertices, data.indices);
                        FAIL() << "Non-finite mesh normal was accepted";
                    }
                    catch (const Error::Exception& error)
                    {
                        EXPECT_EQ(error.code(), Error::Code::RENDER);
                    }
                }
            auto mesh = renderer.createMesh(data.vertices, data.indices);
            observer = mesh;
            data.vertices.clear();
            data.vertices.shrink_to_fit();
            data.indices.clear();
            data.indices.shrink_to_fit();
            ASSERT_TRUE(mesh.valid());
            EXPECT_TRUE(observer.valid());
            EXPECT_FALSE(Mesh{}.valid());
            const auto extent = renderer.beginUiFrame();
            ASSERT_TRUE(extent);
            const std::array draws = {DrawItem{mesh, material, glm::mat4(1.0f)}};
            const auto frame = Demo::makeSceneFrame(*extent, draws);
            renderer.imgui().endFrame();
            const std::array invalidDraws = {DrawItem{mesh, material, glm::mat4(1.0f)},
                                             DrawItem{Mesh{}, material, glm::mat4(1.0f)}};
            auto invalidFrame = frame;
            invalidFrame.draws = invalidDraws;
            EXPECT_THROW(renderer.drawFrame(invalidFrame, {0.2f, 0.3f, 0.4f, 1.0f}), Error::Exception);
            renderer.drawFrame(frame, {0.2f, 0.3f, 0.4f, 1.0f});
            mesh = Mesh{};
            // CPU 数组和原始引用销毁后，各副本仍共享同一次上传结果。
            for (int frameIndex = 0; frameIndex < 6; ++frameIndex)
                EXPECT_GT(drawUiFrame(*window, renderer, observer, material), 0);
            Renderer moved(std::move(renderer));
            EXPECT_THROW(renderer.createMesh({}, {}), Error::Exception);
            EXPECT_GT(drawUiFrame(*window, moved, observer, material), 0);
            // 不显式等待：Renderer 会保护最后一次提交，并在 Device 销毁前释放网格。
        }
        EXPECT_FALSE(observer.valid());
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, MeshFromAnotherRendererIsRejectedBeforeRecording)
    {
        auto firstWindow = createWindow();
        auto secondWindow = createWindow();
        SceneAssets assets;
        {
            Renderer first(*firstWindow, RendererValidationMode::Required);
            const auto firstTexture = first.createTexture(*assets.texture);
            first.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto firstMaterial = first.createMaterial({firstTexture});
            const auto mesh = createCubeMesh(first);
            {
                struct RestoreContext
                {
                    ImGuiContext* previous;
                    ~RestoreContext()
                    {
                        ImGui::SetCurrentContext(previous);
                    }
                } restore{ImGui::GetCurrentContext()};
                // CreateContext 会保留当前已有的上下文；此测试需要单独创建一个上下文。
                ImGui::SetCurrentContext(nullptr);
                Renderer second(*secondWindow, RendererValidationMode::Required);
                second.prepareSceneResources(*assets.vertex, *assets.fragment);
                auto foreignGroup = first.createResources();
                const auto geometry = Demo::makeCubeMesh();
                EXPECT_THROW(second.createMesh(foreignGroup, geometry.vertices, geometry.indices), Error::Exception);
                EXPECT_THROW(second.createTexture(foreignGroup, *assets.texture), Error::Exception);
                EXPECT_THROW(second.createMaterial(foreignGroup, {firstTexture}), Error::Exception);
                EXPECT_THROW(second.createMaterial({firstTexture}), Error::Exception);
                const auto secondMaterial = second.createMaterial({second.createTexture(*assets.texture)});
                const auto ownMesh = createCubeMesh(second);
                const auto extent = second.beginUiFrame();
                ASSERT_TRUE(extent);
                const std::array ownDraws = {DrawItem{ownMesh, secondMaterial, glm::mat4(1.0f)}};
                const auto frame = Demo::makeSceneFrame(*extent, ownDraws);
                second.imgui().endFrame();
                int recorded = 0;
                for (const auto& foreign : {DrawItem{mesh, secondMaterial, glm::mat4(1.0f)},
                                            DrawItem{ownMesh, firstMaterial, glm::mat4(1.0f)}})
                {
                    const std::array invalid{ownDraws[0], foreign};
                    auto foreignFrame = frame;
                    foreignFrame.draws = invalid;
                    EXPECT_THROW(second.drawFrame(foreignFrame, {0.2f, 0.3f, 0.4f, 1.0f}, [&](void*) { ++recorded; }),
                                 Error::Exception);
                }
                EXPECT_EQ(recorded, 0);
                second.drawFrame(frame, {0.2f, 0.3f, 0.4f, 1.0f});
            }
            releaseCpuAssets(assets);
            EXPECT_GT(drawUiFrame(*firstWindow, first, mesh, firstMaterial), 0);
        }
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, FirstZeroFramebufferRestoresUiWithoutExtraResize)
    {
        auto native = createWindow();
        for (const bool prepareScene : {false, true})
        {
            DeferredFramebufferWindow window(*native);
            Renderer renderer(window, RendererValidationMode::Required);
            Mesh mesh;
            Material material;
            if (prepareScene)
            {
                SceneAssets assets;
                renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
                material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
                mesh = createCubeMesh(renderer);
                releaseCpuAssets(assets);
            }
            EXPECT_EQ(ImGui::GetIO().BackendRendererUserData, nullptr);
            const auto skippedLogs = countOccurrences(messages_.str(), "Swapchain ");
            for (int frame = 0; frame < 5; ++frame)
                EXPECT_FALSE(renderer.beginUiFrame());
            EXPECT_EQ(ImGui::GetFrameCount(), 0);
            EXPECT_EQ(countOccurrences(messages_.str(), "Swapchain "), skippedLogs);
            window.drawable = true;
            int recorded = 0;
            size_t steadyLogs = 0;
            for (int frame = 0; frame < 10; ++frame)
            {
                if (prepareScene)
                    recorded += drawUiFrame(window, renderer, mesh, material);
                else
                {
                    submitControlledScene(window, renderer, {}, true);
                    ++recorded;
                }
                if (frame == 0)
                    steadyLogs = countOccurrences(messages_.str(), "Swapchain ");
            }
            EXPECT_GT(recorded, 0);
            EXPECT_GT(steadyLogs, skippedLogs);
            EXPECT_EQ(countOccurrences(messages_.str(), "Swapchain "), steadyLogs);
            EXPECT_NE(ImGui::GetIO().BackendRendererUserData, nullptr);
            EXPECT_TRUE(ImGui::GetIO().Fonts->IsBuilt());
            window.drawable = false;
            EXPECT_FALSE(renderer.beginUiFrame());
            EXPECT_EQ(countOccurrences(messages_.str(), "Swapchain "), steadyLogs);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, VisibleWindowMinimizeRestoreAndResize)
    {
        auto window = createWindow(true);
        FileAudit audit;
        SceneAssets assets({}, std::make_unique<AuditedFileIO>(audit));
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            const auto mesh = createCubeMesh(renderer);
            releaseCpuAssets(assets);
            auto* native = static_cast<GLFWwindow*>(window->getNativeHandle());
            for (int cycle = 0; cycle < 4; ++cycle)
            {
                const auto resizeLogs = countOccurrences(messages_.str(), "Swapchain framebuffer size changed:");
                glfwSetWindowSize(native, 680 + cycle * 20, 400 + cycle * 10);
                EXPECT_GT(drawUiFrame(*window, renderer, mesh, material), 0);
                EXPECT_GT(countOccurrences(messages_.str(), "Swapchain framebuffer size changed:"), resizeLogs);
                glfwIconifyWindow(native);
                for (int attempt = 0; attempt < 100 && !window->isIconified(); ++attempt)
                {
                    window->pollEvents();
                    glfwWaitEventsTimeout(0.005);
                }
                ASSERT_TRUE(window->isIconified());
                for (int frame = 0; frame < 5; ++frame)
                    EXPECT_EQ(drawUiFrame(*window, renderer, mesh, material), 0);
                glfwRestoreWindow(native);
                for (int attempt = 0; attempt < 100 && window->isIconified(); ++attempt)
                {
                    window->pollEvents();
                    glfwWaitEventsTimeout(0.005);
                }
                ASSERT_FALSE(window->isIconified());
                int recorded = 0;
                for (int frame = 0; frame < 10; ++frame)
                    recorded += drawUiFrame(*window, renderer, mesh, material);
                EXPECT_GT(recorded, 0);
                EXPECT_EQ(audit.textureOpens, 1u);
            }
        }
        std::cout << "MINIMIZE_RESTORE_CYCLES=4 PNG_OPENS=" << audit.textureOpens << '\n';
        expectCleanValidation();
    }
    TEST_F(VulkanReviewTest, RapidUiShutdownAfterLastSubmission)
    {
        auto window = createWindow();
        // 分别提交一帧、两帧和三帧，以覆盖不同的关闭边界。
        for (int cycle = 0; cycle < 3; ++cycle)
        {
            SceneAssets assets;
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            const auto mesh = createCubeMesh(renderer);
            releaseCpuAssets(assets);
            int recorded = 0;
            for (int frame = 0; frame < 1 + cycle % 3; ++frame)
                recorded += drawUiFrame(*window, renderer, mesh, material);
            EXPECT_GT(recorded, 0);
            // 此处不显式等待：析构函数必须保护最后一次 GPU 提交。
        }
        expectCleanValidation();
    }
    TEST_F(VulkanReviewTest, ResizeAfterAcquirePreservesFrameAndRebuilds)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            const auto mesh = createCubeMesh(renderer);
            releaseCpuAssets(assets);
            const auto extent = renderer.beginUiFrame();
            ASSERT_TRUE(extent);
            ImGui::Begin("Resize during acquired frame");
            ImGui::Text("Review");
            ImGui::End();
            renderer.imgui().endFrame();
            auto* data = renderer.imgui().drawData();
            const std::array draws = {DrawItem{mesh, material, glm::mat4(1.0f)}};
            renderer.drawFrame(Demo::makeSceneFrame(*extent, draws), {0.1f, 0.2f, 0.3f, 1.0f}, [&](void* commands) {
                renderer.imgui().recordDrawData(data, commands);
                glfwSetWindowSize(static_cast<GLFWwindow*>(window->getNativeHandle()), 710, 430);
            });
            int recorded = 0;
            for (int frame = 0; frame < 10; ++frame)
                recorded += drawUiFrame(*window, renderer, mesh, material);
            EXPECT_GT(recorded, 0);
            EXPECT_NE(messages_.str().find("710x430"), std::string::npos);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, ResizeBeforeSubmissionSkipsStaleProjectionAndUsesNewExtent)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            const auto mesh = createCubeMesh(renderer);
            releaseCpuAssets(assets);
            const auto extent = renderer.beginUiFrame();
            ASSERT_TRUE(extent);
            const std::array draws = {DrawItem{mesh, material, glm::mat4(1.0f)}};
            const auto staleInput = Demo::makeSceneFrame(*extent, draws);
            renderer.imgui().endFrame();
            glfwSetWindowSize(static_cast<GLFWwindow*>(window->getNativeHandle()), 810, 450);
            window->pollEvents();
            int recorded = 0;
            renderer.drawFrame(staleInput, {0.1f, 0.2f, 0.3f, 1.0f}, [&](void*) { ++recorded; });
            EXPECT_EQ(recorded, 0);
            const auto resized = renderer.beginUiFrame();
            ASSERT_TRUE(resized);
            EXPECT_EQ(resized->width, 810u);
            EXPECT_EQ(resized->height, 450u);
            int width = 0;
            int height = 0;
            window->getFramebufferSize(width, height);
            EXPECT_EQ(resized->width, static_cast<uint32_t>(width));
            EXPECT_EQ(resized->height, static_cast<uint32_t>(height));
            renderer.imgui().endFrame();
            renderer.drawFrame(Demo::makeSceneFrame(*resized, draws), {0.1f, 0.2f, 0.3f, 1.0f},
                               [&](void*) { ++recorded; });
            EXPECT_EQ(recorded, 1);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, ApplicationMatricesControlPresentedImageAndResizeProjection)
    {
        auto window = createWindow(true, 640, 480);
        SceneAssets assets(referenceSceneAssetPaths());
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            const auto mesh = createCubeMesh(renderer);
            releaseCpuAssets(assets);
            auto present = [&](const Mesh& selectedMesh, float modelX, float cameraX) {
                // 通过两个 GPU 帧槽测试公开提交路径。调用返回后，
                // 修改应用输入不得改变已经提交的 uniform。
                for (int frame = 0; frame < 8; ++frame)
                {
                    window->pollEvents();
                    const auto extent = renderer.beginUiFrame();
                    if (!extent)
                        throw Error::Exception(Error::Code::RENDER, "Visible matrix review frame was unavailable");
                    std::array draws = {DrawItem{selectedMesh, material,
                                                 glm::translate(glm::mat4(1.0f), glm::vec3(modelX, 0.0f, 0.0f))}};
                    auto input = Demo::makeSceneFrame(*extent, draws);
                    input.camera.view = glm::lookAtRH(glm::vec3(cameraX, 0.0f, 3.0f), glm::vec3(cameraX, 0.0f, 0.0f),
                                                      glm::vec3(0.0f, 1.0f, 0.0f));
                    renderer.imgui().endFrame();
                    renderer.drawFrame(input, {0.2f, 0.24f, 0.28f, 1.0f});
                    draws[0].model = glm::mat4(0.0f);
                    input.camera.projection = glm::mat4(0.0f);
                    glfwWaitEventsTimeout(0.025);
                }
                return WindowCapture::capture(*window);
            };

            const auto first = present(mesh, 0.0f, 0.0f);
            const auto sharedMesh = mesh;
            const auto stable = present(sharedMesh, 0.0f, 0.0f);
            WindowCapture::save(first, "review/frame-input-fixed.ppm");
            const auto baseBounds = WindowCapture::bounds(first);
            ASSERT_GT(baseBounds[2] - baseBounds[0], 100);
            ASSERT_GT(baseBounds[3] - baseBounds[1], 100);
            EXPECT_NEAR(baseBounds[2] - baseBounds[0], baseBounds[3] - baseBounds[1], 2);
            ASSERT_EQ(first.rgba.size(), stable.rgba.size());
            size_t changed = 0;
            // 比较场景像素时，排除 Windows 圆角区域和桌面边框像素。
            for (int y = 8; y < first.height - 8; ++y)
            {
                for (int x = 8; x < first.width - 8; ++x)
                {
                    const size_t pixel = (static_cast<size_t>(y) * first.width + x) * 4;
                    int difference = 0;
                    for (int channel = 0; channel < 3; ++channel)
                        difference += std::abs(int(first.rgba[pixel + channel]) - int(stable.rgba[pixel + channel]));
                    changed += difference > 12;
                }
            }
            EXPECT_LT(changed, 32u);
            // 朝向 +Z 的可见顶行仍应依次为红、绿、蓝、黄。
            const int sampleY = baseBounds[1] + (baseBounds[3] - baseBounds[1]) / 6;
            for (int column = 0; column < 3; ++column)
            {
                const int sampleX = baseBounds[0] + (2 * column + 1) * (baseBounds[2] - baseBounds[0]) / 8;
                const size_t offset = (static_cast<size_t>(sampleY) * first.width + sampleX) * 4;
                for (int channel = 0; channel < 3; ++channel)
                    if (channel != column)
                        EXPECT_GT(int(first.rgba[offset + column]) - int(first.rgba[offset + channel]), 100);
            }

            const auto modelImage = present(mesh, 0.5f, 0.0f);
            const auto cameraImage = present(mesh, 0.0f, 0.5f);
            const auto modelBounds = WindowCapture::bounds(modelImage);
            const auto cameraBounds = WindowCapture::bounds(cameraImage);
            EXPECT_GT(modelBounds[0] - baseBounds[0], 50);
            EXPECT_GT(baseBounds[0] - cameraBounds[0], 50);
            WindowCapture::save(modelImage, "review/frame-input-model.ppm");
            WindowCapture::save(cameraImage, "review/frame-input-camera.ppm");

            glfwSetWindowSize(static_cast<GLFWwindow*>(window->getNativeHandle()), 800, 400);
            const auto resized = present(sharedMesh, 0.0f, 0.0f);
            const auto resizedBounds = WindowCapture::bounds(resized);
            EXPECT_EQ(resized.width, 800);
            EXPECT_EQ(resized.height, 400);
            EXPECT_GT(resizedBounds[2] - resizedBounds[0], 100);
            EXPECT_NEAR(resizedBounds[2] - resizedBounds[0], resizedBounds[3] - resizedBounds[1], 2);
            WindowCapture::save(resized, "review/frame-input-resized.ppm");
            const auto plane = [&] {
                const auto data = Demo::makePlaneMesh();
                return renderer.createMesh(data.vertices, data.indices);
            }();
            const auto planeImage = present(plane, 0.0f, 0.0f);
            const auto planeBounds = WindowCapture::bounds(planeImage);
            const auto planeWidth = planeBounds[2] - planeBounds[0];
            EXPECT_GT(planeWidth, 100);
            EXPECT_LT(planeWidth, resizedBounds[2] - resizedBounds[0] - 20);
            EXPECT_NEAR(planeWidth, planeBounds[3] - planeBounds[1], 2);
            WindowCapture::save(planeImage, "review/mesh-plane.ppm");
            std::cout << "PRODUCTION_FRAME_INPUT: stable_changed_pixels=" << changed
                      << " model_shift=" << modelBounds[0] - baseBounds[0]
                      << " camera_shift=" << cameraBounds[0] - baseBounds[0] << " resized=" << resized.width << 'x'
                      << resized.height << " cube_width=" << resizedBounds[2] - resizedBounds[0]
                      << " plane_width=" << planeWidth << '\n';
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, MultiObjectTransformsStayIndependentAcrossFrameSlotsAndCpuListRelease)
    {
        auto window = createWindow(true, 640, 480);
        FileAudit audit;
        SceneAssets assets({}, std::make_unique<AuditedFileIO>(audit));
        Mesh planeObserver;
        Mesh cubeObserver;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            auto cube = createCubeMesh(renderer);
            auto plane = [&] {
                const auto data = Demo::makePlaneMesh();
                return renderer.createMesh(data.vertices, data.indices);
            }();
            cubeObserver = cube;
            planeObserver = plane;
            releaseCpuAssets(assets);
            const auto scale = glm::scale(glm::mat4(1.0f), glm::vec3(0.55f));
            const auto leftModel = glm::translate(glm::mat4(1.0f), glm::vec3(-0.6f, 0.0f, 0.0f)) * scale;
            std::array<int, 4> fixedBounds{};
            for (int frameIndex = 0; frameIndex < 6; ++frameIndex)
            {
                const float y = frameIndex % 2 == 0 ? 0.25f : -0.25f;
                const auto rightModel = glm::translate(glm::mat4(1.0f), glm::vec3(0.6f, y, 0.0f)) *
                                        glm::rotate(glm::mat4(1.0f), 0.4f, glm::vec3(0.0f, 1.0f, 0.0f)) * scale;
                // 前四帧使用不同网格；最后两帧共享平面网格的上传结果。
                std::vector<DrawItem> draws = {{plane, material, leftModel},
                                               {frameIndex < 4 ? cube : plane, material, rightModel}};
                submitControlledScene(*window, renderer, draws);
                for (auto& draw : draws)
                    draw.model = glm::mat4(0.0f);
                draws.clear();
                draws.shrink_to_fit();
                if (frameIndex == 5)
                {
                    cube = Mesh{};
                    plane = Mesh{};
                }
                // 只等待桌面呈现；正式代码没有增加设备空闲等待。
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                const auto image = WindowCapture::capture(*window);
                const auto left = WindowCapture::bounds(image, 8, image.width / 2);
                const auto right = WindowCapture::bounds(image, image.width / 2, image.width - 8);
                ASSERT_GT(left[2] - left[0], 100);
                ASSERT_GT(right[2] - right[0], 100);
                EXPECT_NEAR((left[0] + left[2]) * 0.5f, image.width * 0.5f - 0.6f * image.height * 0.5f, 2.0f);
                EXPECT_NEAR((right[1] + right[3]) * 0.5f, image.height * (0.5f - y * 0.5f), 2.0f);
                if (frameIndex == 0)
                    fixedBounds = left;
                else
                    EXPECT_EQ(left, fixedBounds);
                if (frameIndex == 0)
                    WindowCapture::save(image, "review/multi-object-independent.ppm");
                if (frameIndex == 5)
                    WindowCapture::save(image, "review/multi-object-shared.ppm");
            }
            EXPECT_EQ(audit.textureOpens, 1u);
            std::cout << "MULTI_OBJECT: UPDATED_FRAMES=6 STATIC_LEFT=1 SHARED_MESH_FRAMES=2 "
                         "CPU_LIST_RELEASED_AFTER_SUBMIT=1 PNG_OPENS="
                      << audit.textureOpens << '\n';
        }
        EXPECT_FALSE(cubeObserver.valid());
        EXPECT_FALSE(planeObserver.valid());
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, MultiObjectDepthOrderAndListCountsPreserveClearUiAndFreshRestore)
    {
        auto native = createWindow(true, 640, 480);
        DeferredFramebufferWindow window(*native);
        window.drawable = true;
        SceneAssets assets(referenceSceneAssetPaths());
        {
            Renderer renderer(window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto texture = renderer.createTexture(*assets.texture);
            const auto material = renderer.createMaterial({texture});
            const auto frontMaterial = renderer.createMaterial({texture, {0.5f, 1.0f, 0.5f}});
            const auto plane = [&] {
                const auto data = Demo::makePlaneMesh();
                return renderer.createMesh(data.vertices, data.indices);
            }();
            releaseCpuAssets(assets);
            FrameExtent lastExtent{};
            const auto present = [&](std::vector<DrawItem> draws) {
                lastExtent = submitControlledScene(window, renderer, draws, true);
                draws.clear();
                draws.shrink_to_fit();
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                return WindowCapture::capture(window);
            };
            const auto sample = [](const WindowCapture::Image& image, int x, int y, int channel) {
                return image.rgba[(static_cast<size_t>(y) * image.width + x) * 4 + channel];
            };
            const auto expectClearAndUi = [&](const WindowCapture::Image& image) {
                for (int channel = 0; channel < 3; ++channel)
                    EXPECT_EQ(sample(image, image.width / 2, image.height / 2, channel), sample(image, 8, 8, channel));
                EXPECT_GT(sample(image, 24, 24, 0), 220);
                EXPECT_LT(sample(image, 24, 24, 1), 30);
                EXPECT_GT(sample(image, 24, 24, 2), 220);
            };
            const auto empty = present({});
            expectClearAndUi(empty);
            const auto scale = glm::scale(glm::mat4(1.0f), glm::vec3(1.4f));
            const DrawItem back{plane, material,
                                glm::translate(glm::mat4(1.0f), glm::vec3(-0.2f, 0.0f, -0.3f)) * scale};
            const DrawItem front{plane, frontMaterial,
                                 glm::translate(glm::mat4(1.0f), glm::vec3(0.2f, 0.0f, 0.3f)) * scale};
            const auto one = present({back});
            const int sampleX = one.width / 2;
            const int sampleY = one.height * 3 / 10;
            EXPECT_GT(sample(one, sampleX, sampleY, 2), 220);
            const auto frontFirst = present({front, back});
            const auto frontLast = present({back, front});
            EXPECT_GT(sample(frontFirst, sampleX, sampleY, 1), 220);
            EXPECT_LT(sample(frontFirst, sampleX, sampleY, 2), 30);
            size_t changed = 0;
            for (int y = 8; y < frontFirst.height - 8; ++y)
            {
                for (int x = 8; x < frontFirst.width - 8; ++x)
                {
                    int difference = 0;
                    for (int channel = 0; channel < 3; ++channel)
                        difference +=
                            std::abs(int(sample(frontFirst, x, y, channel)) - int(sample(frontLast, x, y, channel)));
                    changed += difference > 12;
                }
            }
            EXPECT_EQ(changed, 0u);
            WindowCapture::save(frontFirst, "review/multi-object-depth.ppm");
            std::vector<DrawItem> grid;
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    grid.push_back(
                        {plane, material,
                         glm::translate(glm::mat4(1.0f), glm::vec3((x - 1.5f) * 0.5f, (1.5f - y) * 0.5f, 0.0f)) *
                             glm::scale(glm::mat4(1.0f), glm::vec3(0.25f))});
            const auto many = present(grid);
            for (int y = 0; y < 4; ++y)
            {
                for (int x = 0; x < 4; ++x)
                {
                    const int px = many.width / 2 + static_cast<int>((x - 1.5f) * many.height / 4);
                    const int py = many.height / 2 + static_cast<int>((y - 1.5f) * many.height / 4);
                    int difference = 0;
                    for (int channel = 0; channel < 3; ++channel)
                        difference += std::abs(int(sample(many, px, py, channel)) - int(sample(many, 8, 8, channel)));
                    EXPECT_GT(difference, 40) << "Missing grid object " << x << ',' << y;
                }
            }
            WindowCapture::save(many, "review/multi-object-grid.ppm");
            window.drawable = false;
            renderer.drawFrame(controlledFrame(lastExtent, grid), {0.2f, 0.24f, 0.28f, 1.0f});
            grid.clear();
            window.drawable = true;
            const auto restoredEmpty = present({});
            expectClearAndUi(restoredEmpty);
            WindowCapture::save(restoredEmpty, "review/multi-object-empty-ui.ppm");
            std::cout << "MULTI_OBJECT: COUNTS=0,1,2,16,0 DEPTH_ORDER_CHANGED_PIXELS=" << changed
                      << " ALL_16_CENTERS=1 EMPTY_UI=1 SKIPPED_LIST_NOT_RETAINED=1\n";
        }
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, DebugMessengerPreservesInjectedWarningAndError)
    {
        auto window = createWindow();
        {
            VulkanInstance instance(requiredExtensions(), ValidationMode::Required);
            const auto submit = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
                vkGetInstanceProcAddr(instance.handle(), "vkSubmitDebugUtilsMessageEXT"));
            ASSERT_NE(submit, nullptr);
            VkDebugUtilsMessengerCallbackDataEXT data{};
            data.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT;
            data.pMessageIdName = "INCUBATOR_REVIEW_INJECTION";
            data.pMessage = "Review injected warning";
            submit(instance.handle(), VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data);
            data.pMessage = "Review injected error with operation/VkResult evidence";
            submit(instance.handle(), VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data);
        }
        EXPECT_NE(messages_.str().find("warning|Review injected warning"), std::string::npos);
        EXPECT_NE(messages_.str().find("error|Review injected error with operation/VkResult evidence"),
                  std::string::npos);
    }
    TEST_F(VulkanReviewTest, ValidationModesWithInstalledLayer)
    {
        auto window = createWindow();
        for (const auto mode : {ValidationMode::Optional, ValidationMode::Required, ValidationMode::Disabled})
        {
            VulkanInstance instance(requiredExtensions(), mode);
            EXPECT_EQ(instance.validationEnabled(), mode != ValidationMode::Disabled);
        }
        EXPECT_EQ(messages_.str().find("warning|"), std::string::npos);
        EXPECT_EQ(messages_.str().find("error|"), std::string::npos);
    }

    TEST_F(VulkanReviewTest, MissingLayerOptionalWarnsAndRequiredFails)
    {
        auto window = createWindow();
        EmptyLayerSearch missing;
        uint32_t count = 0;
        ASSERT_EQ(vkEnumerateInstanceLayerProperties(&count, nullptr), VK_SUCCESS);
        std::vector<VkLayerProperties> layers(count);
        ASSERT_EQ(vkEnumerateInstanceLayerProperties(&count, layers.data()), VK_SUCCESS);
        ASSERT_FALSE(std::any_of(layers.begin(), layers.end(), [](const auto& layer) {
            return std::string_view(layer.layerName) == "VK_LAYER_KHRONOS_validation";
        }));
        {
            VulkanInstance optional(requiredExtensions(), ValidationMode::Optional);
            EXPECT_FALSE(optional.validationEnabled());
        }
        EXPECT_NE(messages_.str().find("continuing without validation"), std::string::npos);
        try
        {
            VulkanInstance required(requiredExtensions(), ValidationMode::Required);
            FAIL() << "Required mode accepted a missing layer";
        }
        catch (const Error::Exception& error)
        {
            EXPECT_NE(error.message().find("Required Vulkan validation component is unavailable"), std::string::npos);
        }
        VulkanInstance disabled(requiredExtensions(), ValidationMode::Disabled);
        EXPECT_FALSE(disabled.validationEnabled());
    }

    TEST_F(VulkanReviewTest, ImGuiRejectsSingleImageBeforeBackendAllocation)
    {
        auto window = createWindow();
        {
            VulkanInstance instance(requiredExtensions(), ValidationMode::Required);
            GlfwSurface surface(instance, *window);
            VulkanDevice device(instance, surface);
            Integration::ImGuiRenderer adapter(*window);
            try
            {
                adapter.initializeVulkan(instance.handle(), device.physicalDevice(), device.handle(),
                                         device.queueFamilies().graphics, device.graphicsQueue(),
                                         VK_FORMAT_B8G8R8A8_UNORM, device.findDepthFormat(), 1);
                FAIL() << "ImGui accepted a single swapchain image";
            }
            catch (const Error::Exception& error)
            {
                EXPECT_NE(error.message().find("at least two swapchain images; received 1"), std::string::npos);
            }
            EXPECT_EQ(ImGui::GetIO().BackendRendererUserData, nullptr);
            adapter.initializeVulkan(instance.handle(), device.physicalDevice(), device.handle(),
                                     device.queueFamilies().graphics, device.graphicsQueue(), VK_FORMAT_B8G8R8A8_UNORM,
                                     device.findDepthFormat(), 2);
            EXPECT_NE(ImGui::GetIO().BackendRendererUserData, nullptr);
            EXPECT_TRUE(ImGui::GetIO().Fonts->IsBuilt());
        }
        expectCleanValidation();
    }
}  // namespace
