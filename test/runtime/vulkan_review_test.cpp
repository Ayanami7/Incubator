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

    glm::vec3 linearFromSrgb(glm::vec3 v)
    {
        for (int c = 0; c < 3; ++c)
            v[c] = v[c] <= 0.04045f ? v[c] / 12.92f : std::pow((v[c] + 0.055f) / 1.055f, 2.4f);
        return v;
    }

    void expectLinearPixel(std::span<const uint8_t> pixels, glm::vec3 linear, uint32_t x = 192, uint32_t y = 128,
                           uint32_t width = 384)
    {
        const size_t off = (size_t(y) * width + x) * 4;
        ASSERT_LT(off + 3, pixels.size());
        for (int c = 0; c < 3; ++c)
        {
            const double v = std::clamp(double(linear[c]), 0.0, 1.0);
            const double e = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
            EXPECT_NEAR(pixels[off + c], e * 255, 2) << "channel=" << c;
        }
        EXPECT_EQ(pixels[off + 3], 255);
    }

    std::vector<const char*> requiredExtensions()
    {
        auto r = GlfwSurface::requiredInstanceExtensions();
        r.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        r.push_back(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        return r;
    }

    size_t countOccurrences(std::string_view messages, std::string_view value)
    {
        size_t count = 0;
        for (size_t pos = messages.find(value); pos != std::string_view::npos; pos = messages.find(value, pos + value.size()))
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
            for (size_t i = 0; i < 2; ++i)
            {
                const auto* cur = std::getenv(names_[i]);
                previous_[i] = cur ? cur : "";
                _putenv_s(names_[i], empty.string().c_str());
            }
        }
        ~EmptyLayerSearch() { _putenv_s(names_[0], previous_[0].c_str()); _putenv_s(names_[1], previous_[1].c_str()); }

    private:
        const std::array<const char*, 2> names_ = {"VK_LAYER_PATH", "VK_IMPLICIT_LAYER_PATH"};
        std::array<std::string, 2> previous_;
    };

    using SceneAssets = Demo::SceneAssets;
    Demo::SceneAssetPaths referenceSceneAssetPaths()
    {
        Demo::SceneAssetPaths p;
        p.texture = "textures/reference_rgba.png";
        return p;
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
                if (const auto id = registry.lookup(type, path); id.valid() && (registry.refCount(id) != 0 || registry.getObject(id)))
                    audit_.cpuReleasedBeforeDestroy = false;
        }
        bool exists(const std::filesystem::path& path) override { return inner_->exists(path); }
        uint64_t getFileSize(const std::filesystem::path& path) override { return inner_->getFileSize(path); }
        bool remove(const std::filesystem::path& path) override { return inner_->remove(path); }
        std::unique_ptr<Incubator::FileIO::FileHandle> open(const std::filesystem::path& p, Incubator::FileIO::OpenMode m,
                                                            Incubator::FileIO::ShareMode s) override
        { if (p.extension() == ".png") ++audit_.textureOpens; return inner_->open(p, m, s); }
        std::vector<Incubator::FileIO::DirectoryEntry> listDirectory(const std::filesystem::path& p) override { return inner_->listDirectory(p); }
        bool directoryExists(const std::filesystem::path& p) override { return inner_->directoryExists(p); }
        bool createDirectory(const std::filesystem::path& p) override { return inner_->createDirectory(p); }
        bool removeDirectory(const std::filesystem::path& p) override { return inner_->removeDirectory(p); }

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
            if (const auto id = registry.lookup(type, path); id.valid())
                EXPECT_TRUE(registry.refCount(id) == 0u && registry.getObject(id) == nullptr);
        EXPECT_FALSE(Resource::ResourceManager::instance().hasLoader(Resource::ResourceType::SHADER) ||
                     Resource::ResourceManager::instance().hasLoader(Resource::ResourceType::TEXTURE));
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
        bool shouldClose() const override { return native_.shouldClose(); }
        void setShouldClose(bool value) override { native_.setShouldClose(value); }
        void pollEvents() override { native_.pollEvents(); }
        void getFramebufferSize(int& width, int& height) const override
        {
            if (throwOnSize)
                throw Error::Exception(Error::Code::RENDER, "Injected framebuffer query failure");
            if (drawable)
                native_.getFramebufferSize(width, height);
            else
                width = height = 0;
        }
        void getWindowSize(int& width, int& height) const override { native_.getWindowSize(width, height); }
        void setTitle(std::string_view title) override { native_.setTitle(title); }
        bool isIconified() const override { return native_.isIconified(); }
        float getContentScale() const override { return native_.getContentScale(); }
        void* getNativeHandle() const override { return native_.getNativeHandle(); }
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
            return Window::createPlatformWindow({"Incubator Vulkan review", width, height, visible});
        }

        Mesh createCubeMesh(Renderer& renderer)
        {
            const auto d = Demo::makeCubeMesh();
            return renderer.createMesh(d.vertices, d.indices);
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
            const auto scale = glm::scale(glm::mat4(1), glm::vec3(0.6f));
            const std::array draws = {
                DrawItem{mesh, material, glm::translate(glm::mat4(1), {-0.65f, 0, 0}) * scale},
                DrawItem{mesh, material, glm::translate(glm::mat4(1), {0.65f, 0, 0}) * scale}};
            renderer.drawFrame(Demo::makeSceneFrame(*extent, draws), {0.12f, 0.16f, 0.22f, 1}, [&](void* commands) {
                ++recorded;
                renderer.imgui().recordDrawData(data, commands);
            });
            return recorded;
        }

        FrameInput controlledFrame(FrameExtent extent, std::span<const DrawItem> draws)
        {
            const float aspect = float(extent.width) / extent.height;
            auto projection = glm::orthoRH_ZO(-aspect, aspect, -1.0f, 1.0f, 0.1f, 10.0f);
            projection[1][1] *= -1.0f;
            return {extent, {glm::lookAtRH(glm::vec3(0, 0, 3), glm::vec3(0), glm::vec3(0, 1, 0)), projection}, draws};
        }

        FrameExtent submitControlledScene(Window::Window& window, Renderer& renderer, std::span<const DrawItem> draws,
                                          bool withUi = false)
        {
            window.pollEvents();
            const auto extent = renderer.beginUiFrame();
            if (!extent)
                throw Error::Exception(Error::Code::RENDER, "Controlled scene requires a drawable frame");
            if (withUi)
                ImGui::GetBackgroundDrawList()->AddRectFilled({16, 16}, {48, 48}, IM_COL32(255, 0, 255, 255));
            renderer.imgui().endFrame();
            auto* ui = renderer.imgui().drawData();
            renderer.drawFrame(controlledFrame(*extent, draws), {0.2f, 0.24f, 0.28f, 1}, [&](void* commands) {
                if (withUi)
                    renderer.imgui().recordDrawData(ui, commands);
            });
            return *extent;
        }

        void expectInteriorEqual(const WindowCapture::Image& a, const WindowCapture::Image& b)
        {
            ASSERT_TRUE(a.width == b.width && a.height == b.height && a.width > 16 && a.height > 16);
            for (int y = 8; y < a.height - 8; ++y)
                for (int x = 8; x < a.width - 8; ++x)
                    for (int c = 0; c < 3; ++c)
                    {
                        const size_t i = (size_t(y) * a.width + x) * 4 + c;
                        ASSERT_EQ(a.rgba[i], b.rgba[i]) << "at (" << x << "," << y << ") ch=" << c;
                    }
        }

        void expectCleanValidation()
        {
            const auto m = messages_.str();
            EXPECT_NE(m.find("validation: enabled"), std::string::npos);
            EXPECT_EQ(m.find("warning|"), std::string::npos) << m;
            EXPECT_EQ(m.find("error|"), std::string::npos) << m;
        }

        std::filesystem::path previousPath_;
        std::ostringstream messages_;
    };

    TEST_F(VulkanReviewTest, UInt32IndexDrawUsesVertexAboveUint16Range)
    {
        auto window = createWindow(true, 640, 480);
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            std::vector<MeshVertex> vertices(65537, MeshVertex{{4, 4, 0}, {0, 0}});
            vertices[0] = {{-0.65f, -0.55f, 0}, {0, 0}};
            vertices[1] = {{0.65f, -0.55f, 0}, {1, 0}};
            vertices[65536] = {{0, 0.65f, 0}, {0.5f, 1}};
            const auto mesh = renderer.createMesh(vertices, std::array<uint32_t, 3>{0, 1, 65536});
            releaseCpuAssets(assets);
            const std::array draws{DrawItem{mesh, material, glm::mat4(1)}};
            for (int f = 0; f < 6; ++f, glfwWaitEventsTimeout(0.025))
                submitControlledScene(*window, renderer, draws);
            const auto bounds = WindowCapture::bounds(WindowCapture::capture(*window));
            EXPECT_GT(bounds[2] - bounds[0], 100);
            EXPECT_GT(bounds[3] - bounds[1], 100);
        }
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, IndependentTexturesShareUploadsAndExpireAfterRendererMoveAndClose)
    {
        Log::Registry::get("render.vulkan").setLevel(Log::Level::DEBUG);
        auto window = createWindow(true, 640, 480);
        Texture reference, direction;
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
        FileAudit firstAudit, secondAudit;
        secondAudit.paths.texture = texturePath;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            Material material;
            {
                SceneAssets assets({}, std::make_unique<AuditedFileIO>(firstAudit));
                reference = renderer.createTexture(*assets.texture);
                releaseCpuAssets(assets);
            }
            {
                SceneAssets assets(secondAudit.paths, std::make_unique<AuditedFileIO>(secondAudit));
                direction = renderer.createTexture(*assets.texture);
                const auto shared = direction;
                renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
                material = renderer.createMaterial({shared});
                releaseCpuAssets(assets);
            }
            EXPECT_TRUE(firstAudit.cpuReleasedBeforeDestroy && secondAudit.cpuReleasedBeforeDestroy);
            EXPECT_TRUE(reference.valid() && direction.valid() && !Texture{}.valid());
            Renderer moved(std::move(renderer));
            EXPECT_THROW(renderer.createTexture(Resource::TextureResource({}, 1, 1, {255, 255, 255, 255})),
                         Error::Exception);
            const auto data = Demo::makePlaneMesh();
            const std::array draws{DrawItem{moved.createMesh(data.vertices, data.indices), material, glm::mat4(1)}};
            for (int f = 0; f < 6; ++f)
            {
                submitControlledScene(*window, moved, draws);
                glfwWaitEventsTimeout(0.025);
            }
            const auto image = WindowCapture::capture(*window);
            const auto b = WindowCapture::bounds(image);
            ASSERT_GT(b[2] - b[0], 100);
            const size_t offset =
                (static_cast<size_t>(b[1] + (b[3] - b[1]) / 8) * image.width + b[0] + (b[2] - b[0]) / 6) * 4;
            for (int c = 0; c < 3; ++c)
                EXPECT_GT(image.rgba[offset + c], 220);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline"), 1u);
            EXPECT_EQ(firstAudit.textureOpens, 1u);
            EXPECT_EQ(secondAudit.textureOpens, 1u);
        }
        EXPECT_FALSE(reference.valid() || direction.valid());
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
            int recorded = 0;
            EXPECT_THROW(renderer.drawFrame(controlledFrame(*extent, std::array{DrawItem{mesh, {}, glm::mat4(1)}}),
                                            {0.2f, 0.24f, 0.28f, 1}, [&](void*) { ++recorded; }),
                         Error::Exception);
            EXPECT_EQ(recorded, 0);
            renderer.drawFrame(controlledFrame(*extent, {}), {0.2f, 0.24f, 0.28f, 1});
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

    TEST_F(VulkanReviewTest, SamplerAndMipMaterialsSelectFiltersAndLevelsAfterCpuRelease)
    {
        Log::Registry::get("render").setLevel(Log::Level::DEBUG);
        Log::Registry::get("render.vulkan").setLevel(Log::Level::DEBUG);
        auto window = createWindow(false, 640, 480);
        SceneAssets assets(referenceSceneAssetPaths());
        std::array<Texture, 2> texObs;
        std::array<Material, 8> matObs{};
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            texObs[0] = renderer.createTexture(Resource::TextureResource(
                {}, 2, 2, {0, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 0, 255, 255, 255, 255}));
            std::vector<uint8_t> mipPixels(4 * 4 * 4);
            for (size_t i = 0; i < 16; ++i)
                for (int c = 0; c < 4; ++c)
                    mipPixels[i * 4 + c] = (i % 4 >= 2) ? 255 : 0;
            const Resource::TextureResource mipSrc({}, 4, 4, std::move(mipPixels));
            EXPECT_THROW(renderer.createTexture(mipSrc, static_cast<TextureMipMode>(99)), Error::Exception);
            texObs[1] = renderer.createTexture(mipSrc, TextureMipMode::FullChain);

            MaterialDesc desc{texObs[0]};
            EXPECT_EQ(desc.sampler, SamplerDesc{});
            auto invalid = desc;
            invalid.sampler.minFilter = static_cast<TextureFilter>(99);
            EXPECT_THROW(renderer.createMaterial(invalid), Error::Exception);
            invalid = desc;
            invalid.sampler.mipFilter = static_cast<MipFilter>(99);
            EXPECT_THROW(renderer.createMaterial(invalid), Error::Exception);

            const std::array<SamplerDesc, 5> samplerModes{
                SamplerDesc{}, {TextureFilter::Nearest, TextureFilter::Linear, MipFilter::BaseLevel},
                {TextureFilter::Linear, TextureFilter::Nearest, MipFilter::BaseLevel},
                {TextureFilter::Linear, TextureFilter::Linear, MipFilter::Nearest},
                {TextureFilter::Linear, TextureFilter::Linear, MipFilter::Linear}};
            for (size_t i = 0; i < samplerModes.size(); ++i)
                matObs[i] = renderer.createMaterial({texObs[0], {1, 1, 1}, samplerModes[i]});
            const std::array mipModes{MipFilter::BaseLevel, MipFilter::Nearest, MipFilter::Linear};
            for (size_t i = 0; i < mipModes.size(); ++i)
                matObs[5 + i] = renderer.createMaterial(
                    {texObs[1], {1, 1, 1}, {TextureFilter::Nearest, TextureFilter::Nearest, mipModes[i]}});
            releaseCpuAssets(assets);

            const std::array<uint32_t, 6> indices{0, 1, 2, 2, 3, 0};
            auto quadDraws = [&](float u0, float u1, const Material& base, const Material& alt) {
                const std::array<MeshVertex, 4> v{{{{-1, -1, 0.5f}, {u0, 0.5f}},
                                                   {{0, -1, 0.5f}, {u1, 0.5f}},
                                                   {{0, 1, 0.5f}, {u1, 0.5f}},
                                                   {{-1, 1, 0.5f}, {u0, 0.5f}}}};
                const auto mesh = renderer.createMesh(v, indices);
                return std::array{DrawItem{mesh, base, glm::mat4(1)},
                                  DrawItem{mesh, alt, glm::translate(glm::mat4(1), glm::vec3(1, 0, 0))}};
            };
            auto captureQuad = [&](const std::array<DrawItem, 2>& draws) {
                std::vector<uint8_t> pixels;
                Detail::RendererReviewAccess::withFrame(
                    renderer, FrameInput{{256, 256}, {glm::mat4(1), glm::mat4(1)}, draws},
                    [&](const auto& d, const auto& p, auto fs, auto nd) {
                        pixels = TextureGpuReview::captureDraws(d, p, fs, nd, {0, 0, 256, 256, 0, 1});
                    });
                return pixels;
            };

            for (bool minify : {false, true})
                for (size_t i = 1; i < 5; ++i)
                {
                    const auto px = captureQuad(quadDraws(minify ? -128.0f : 0.5f, minify ? 0.0f : 0.5f,
                                                          matObs[0], matObs[i]));
                    const bool linear = i >= 3 || (minify ? i == 2 : i == 1);
                    for (int c = 0; c < 3; ++c)
                    {
                        EXPECT_NEAR(px[(128 * 384 + 64) * 4 + c], 255, 1);
                        EXPECT_NEAR(px[(128 * 384 + 192) * 4 + c], linear ? 188 : 255, 1);
                    }
                    EXPECT_EQ(px[(128 * 384 + 192) * 4 + 3], 255);
                }

            for (float lod : {0.0f, 1.25f, 1.75f, 10.0f})
            {
                const float step = std::exp2(lod) / 4.0f;
                const float u0 = 0.625f - 64.5f * step, u1 = u0 + 128.0f * step;
                for (size_t i = 1; i < 3; ++i)
                {
                    const auto draws = quadDraws(u0, u1, matObs[5], matObs[5 + i]);
                    const auto px = captureQuad(draws);
                    const float linear = i == 1 ? (lod < 1.5f ? 1.0f : 0.5f) : 1.0f - 0.5f * std::clamp(lod - 1.0f, 0.0f, 1.0f);
                    const float encoded = 255.0f * (1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f);
                    for (int c = 0; c < 3; ++c)
                    {
                        EXPECT_NEAR(px[(128 * 384 + 64) * 4 + c], 255, 1);
                        EXPECT_NEAR(px[(128 * 384 + 192) * 4 + c], encoded, 2);
                    }
                    if (lod == 1.75f && i == 1)
                        glfwSetWindowSize(static_cast<GLFWwindow*>(window->getNativeHandle()), 800, 600);
                    window->pollEvents();
                    const auto extent = renderer.beginUiFrame();
                    ASSERT_TRUE(extent);
                    renderer.imgui().endFrame();
                    renderer.drawFrame({*extent, {glm::mat4(1), glm::mat4(1)}, draws}, {0.2f, 0.24f, 0.28f, 1});
                }
            }
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline:"), 1u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created material:"), 8u);
        }
        for (const auto& t : texObs)
            EXPECT_FALSE(t.valid());
        for (const auto& m : matObs)
            EXPECT_FALSE(m.valid());
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, GeneratedMipExtentsAndLinearSrgbAlphaMatchReadback)
    {
        auto window = createWindow();
        {
            VulkanInstance instance(requiredExtensions(), ValidationMode::Required);
            GlfwSurface surface(instance, *window);
            VulkanDevice device(instance, surface);
            for (const auto& chain : std::vector<std::vector<VkExtent2D>>{
                     {{1, 1}}, {{1, 9}, {1, 4}, {1, 2}, {1, 1}}, {{9, 1}, {4, 1}, {2, 1}, {1, 1}},
                     {{3, 5}, {1, 2}, {1, 1}}, {{4, 4}, {2, 2}, {1, 1}}})
            {
                std::vector<uint8_t> pixels(chain[0].width * chain[0].height * 4);
                for (size_t i = 0; i < pixels.size(); ++i)
                    pixels[i] = std::array<uint8_t, 4>{64, 128, 192, 85}[i % 4];
                const auto image = VulkanImage::uploadRgba8(
                    device, chain[0], VK_FORMAT_R8G8B8A8_SRGB, pixels,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, TextureMipMode::FullChain);
                ASSERT_EQ(image->mipLevels(), chain.size());
                for (uint32_t level = 0; level < chain.size(); ++level)
                {
                    const auto px = TextureGpuReview::captureMip(device, *image, level, chain[level]);
                    ASSERT_EQ(px.size(), chain[level].width * chain[level].height * 4);
                    for (size_t i = 0; i < px.size(); ++i)
                        EXPECT_NEAR(px[i], (std::array<uint8_t, 4>{64, 128, 192, 85}[i % 4]), 1);
                }
            }
            const std::array<uint8_t, 16> bw{0, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 0, 255, 255, 255, 255};
            const auto image = VulkanImage::uploadRgba8(device, {2, 2}, VK_FORMAT_R8G8B8A8_SRGB, bw,
                                                        VK_IMAGE_USAGE_SAMPLED_BIT, TextureMipMode::FullChain);
            ASSERT_EQ(image->mipLevels(), 2u);
            const auto mip = TextureGpuReview::captureMip(device, *image, 1, {1, 1});
            for (int c = 0; c < 3; ++c)
                EXPECT_NEAR(mip[c], 188, 1);
            EXPECT_NEAR(mip[3], 128, 1);
            EXPECT_EQ(VulkanImage::uploadRgba8(device, {2, 2}, VK_FORMAT_R8G8B8A8_SRGB, bw, VK_IMAGE_USAGE_SAMPLED_BIT)
                          ->mipLevels(),
                      1u);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, MaterialsCopyParametersShareTexturesAndPerItemDraws)
    {
        Log::Registry::get("render").setLevel(Log::Level::DEBUG);
        Log::Registry::get("render.vulkan").setLevel(Log::Level::DEBUG);
        auto window = createWindow(true, 640, 480);
        FileAudit referenceAudit{referenceSceneAssetPaths()}, directionAudit{referenceSceneAssetPaths()};
        directionAudit.paths.texture = "textures/direction_rgba.png";
        Texture textureObserver;
        std::array<Material, 3> observers;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            Material white, half, direction;
            {
                SceneAssets assets(referenceAudit.paths, std::make_unique<AuditedFileIO>(referenceAudit));
                renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
                textureObserver = renderer.createTexture(*assets.texture);
                MaterialDesc desc{textureObserver};
                EXPECT_EQ(desc.color, (std::array{1.0f, 1.0f, 1.0f}));
                white = renderer.createMaterial(desc);
                desc.color = {0.25f, 0.5f, 1.0f};
                half = renderer.createMaterial(desc);
                EXPECT_FALSE(Material{}.valid());
                for (int c = 0; c < 3; ++c)
                    for (const float v : {-0.1f, 1.1f, std::numeric_limits<float>::infinity(),
                                          std::numeric_limits<float>::quiet_NaN()})
                    {
                        auto invalid = desc;
                        invalid.color[c] = v;
                        EXPECT_THROW(renderer.createMaterial(invalid), Error::Exception);
                    }
                desc = {};
                releaseCpuAssets(assets);
            }
            {
                SceneAssets assets(directionAudit.paths, std::make_unique<AuditedFileIO>(directionAudit));
                direction = renderer.createMaterial({renderer.createTexture(*assets.texture)});
                releaseCpuAssets(assets);
            }
            observers = {white, half, direction};
            const auto pd = Demo::makePlaneMesh();
            auto plane = renderer.createMesh(pd.vertices, pd.indices);
            auto present = [&](const Material& selected) {
                for (int f = 0; f < 6; ++f, glfwWaitEventsTimeout(0.03))
                    submitControlledScene(*window, renderer, std::array{DrawItem{plane, selected, glm::mat4(1)}});
                return WindowCapture::capture(*window);
            };
            (void)present(white);
            const auto original = present(white);
            white = half = direction = {};
            expectInteriorEqual(original, present(observers[0]));
            const auto image = present(observers[1]);
            const auto b = WindowCapture::bounds(image);
            ASSERT_GT(b[2] - b[0], 100);
            const auto offset = (size_t(b[1] + (b[3] - b[1]) / 2) * image.width + b[0] + 7 * (b[2] - b[0]) / 8) * 4;
            EXPECT_GT(int(image.rgba[offset + 1]), int(image.rgba[offset]) + 20);
            EXPECT_GT(int(image.rgba[offset + 2]), int(image.rgba[offset + 1]) + 20);
            for (int f = 0; f < 4; ++f)
            {
                const bool sw = f % 2 != 0;
                std::vector<DrawItem> draws = {
                    {plane, observers[sw ? 1 : 0], glm::translate(glm::mat4(1), {-0.65f, 0.45f, 0}) * glm::scale(glm::mat4(1), glm::vec3(0.65f))},
                    {plane, observers[sw ? 0 : 1], glm::translate(glm::mat4(1), {0.65f, 0.45f, 0}) * glm::scale(glm::mat4(1), glm::vec3(0.65f))},
                    {plane, observers[2], glm::translate(glm::mat4(1), {0.65f, -0.45f, 0}) * glm::scale(glm::mat4(1), glm::vec3(0.65f))}};
                if (sw)
                    std::reverse(draws.begin(), draws.end());
                submitControlledScene(*window, renderer, draws, true);
                draws.clear();
                glfwWaitEventsTimeout(0.06);
                const auto img = WindowCapture::capture(*window);
                const auto px = [&](float x, float y, int c) {
                    return img.rgba[(size_t(img.height / 2 - int(y * img.height / 2)) * img.width + img.width / 2 +
                                     int(x * img.height / 2)) * 4 + c];
                };
                EXPECT_NEAR(px(-0.65f + 0.24375f, 0.45f, 0), sw ? 137 : 255, 14);
                EXPECT_NEAR(px(0.65f + 0.24375f, 0.45f, 0), sw ? 255 : 137, 14);
                EXPECT_GT(px(0.65f + 0.21667f, -0.45f + 0.08125f, 0), 220);
                EXPECT_LT(px(0.65f + 0.21667f, -0.45f + 0.08125f, 1), 30);
            }
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline"), 1u);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created material:"), 3u);
            EXPECT_TRUE(referenceAudit.cpuReleasedBeforeDestroy && directionAudit.cpuReleasedBeforeDestroy);
        }
        EXPECT_FALSE(textureObserver.valid());
        for (const auto& material : observers)
            EXPECT_FALSE(material.valid());
        expectReleased(referenceAudit.paths);
        expectReleased(directionAudit.paths);
        expectCleanValidation();
    }

    static_assert(!std::is_copy_constructible_v<GpuResources>);
    static_assert(std::is_nothrow_move_constructible_v<GpuResources>);

    TEST_F(VulkanReviewTest, ScopedResourcesPreserveDependenciesRollbackAndSharedLateRelease)
    {
        auto window = createWindow();
        SceneAssets assets;
        std::shared_ptr<GpuResources> external;
        Mesh extMesh;
        Texture extTex;
        Material extMat;
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
                auto textures = renderer.createResources(), bindings = renderer.createResources();
                const auto tex = renderer.createTexture(textures, *assets.texture);
                const auto mat = renderer.createMaterial(bindings, {tex});
                const auto mesh = renderer.createMesh(bindings, geometry.vertices, geometry.indices);
                textures.reset();
                EXPECT_TRUE(tex.valid());
                expectCounts(2, 2, 2);
                auto moved = std::move(bindings);
                auto assigned = renderer.createResources();
                const auto discarded = renderer.createMesh(assigned, geometry.vertices, geometry.indices);
                assigned = std::move(moved);
                EXPECT_FALSE(bindings.valid() || discarded.valid() || moved.valid());
                EXPECT_TRUE(mesh.valid() && mat.valid());
                assigned.reset();
                EXPECT_FALSE(mesh.valid() || mat.valid() || tex.valid());
                EXPECT_THROW(renderer.drawFrame(controlledFrame({640, 360}, std::array{DrawItem{mesh, mat, glm::mat4(1)}}),
                                                {0, 0, 0, 1}),
                             Error::Exception);
            }
            expectCounts(1, 1, 1);
            for (int stage = 0; stage != 3; ++stage)
            {
                Mesh iMesh;
                Texture iTex;
                Material iMat;
                EXPECT_THROW(
                    {
                        auto attempt = renderer.createResources();
                        iMesh = renderer.createMesh(attempt, geometry.vertices, geometry.indices);
                        if (stage >= 1)
                            iTex = renderer.createTexture(attempt, *assets.texture);
                        if (stage >= 2)
                            iMat = renderer.createMaterial(attempt, {iTex});
                        throw std::runtime_error("interrupted");
                    },
                    std::runtime_error);
                EXPECT_FALSE(iMesh.valid() || iTex.valid() || iMat.valid());
                expectCounts(1, 1, 1);
            }
            EXPECT_GT(drawUiFrame(*window, renderer, directMesh, directMaterial), 0);
            external = std::make_shared<GpuResources>(renderer.createResources());
            extMesh = renderer.createMesh(*external, geometry.vertices, geometry.indices);
            extTex = renderer.createTexture(*external, *assets.texture);
            extMat = renderer.createMaterial(*external, {extTex});
            releaseCpuAssets(assets);
            Renderer moved(std::move(renderer));
            EXPECT_TRUE(external->valid() && extMesh.valid());
            EXPECT_GT(drawUiFrame(*window, moved, extMesh, extMat), 0);
        }
        EXPECT_FALSE(external->valid() || extMesh.valid() || extTex.valid() || extMat.valid());
        external.reset();
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
                Mesh caster = shadow ? renderer.createMesh(resources, geometry.vertices, geometry.indices) : Mesh{};
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
                EXPECT_TRUE(mesh.valid() && texture.valid() && material.valid());
                renderer.prepareSceneTarget({384, 256});
                EXPECT_TRUE(mesh.valid());
                if (shadow)
                {
                    EXPECT_TRUE(caster.valid());
                    Detail::RendererReviewAccess::withSceneOutput(
                        renderer, output, [&](const auto& device, const auto& image) {
                            EXPECT_LT(TextureGpuReview::captureMip(device, image, 0, {384, 256})[(128u * 384u + 192u) * 4u], 5u);
                        });
                }
                else
                    Detail::RendererReviewAccess::waitForFrames(renderer);
                EXPECT_FALSE(mesh.valid() || material.valid() || texture.valid() || caster.valid());
                const auto counts = Detail::RendererReviewAccess::resourceCounts(renderer);
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
            EXPECT_THROW(renderer.prepareSceneTarget({320, 240}), Error::Exception);
            EXPECT_THROW(renderer.drawSceneTarget(output, {}, {0, 0, 0, 1}), Error::Exception);
            EXPECT_THROW((void)renderer.sceneImage(output), Error::Exception);
            EXPECT_THROW(renderer.createResources(), Error::Exception);
            EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            EXPECT_THROW(renderer.prepareSceneResources(*assets.vertex, *assets.fragment), Error::Exception);
            EXPECT_THROW(renderer.drawFrame(controlledFrame({320, 240}, {}), {0, 0, 0, 1}), Error::Exception);
            EXPECT_NE(moved.sceneImage(output), 0u);
            moved.drawSceneTarget(output, controlledFrame(output.extent(), {}), {0, 0, 0, 1});
            const auto mesh = createCubeMesh(moved);
            releaseCpuAssets(assets);
            const auto extent = moved.beginUiFrame();
            ASSERT_TRUE(extent);
            moved.imgui().endFrame();
            int recorded = 0;
            for (const auto& invalid : {DrawItem{mesh, {}, glm::mat4(1.0f)}, DrawItem{mesh, expired, glm::mat4(1.0f)},
                                        DrawItem{expiredMesh, observer, glm::mat4(1.0f)}})
                EXPECT_THROW(moved.drawFrame(controlledFrame(*extent, std::array{DrawItem{mesh, observer, glm::mat4(1)}, invalid}),
                                            {0.2f, 0.24f, 0.28f, 1}, [&](void*) { ++recorded; }),
                             Error::Exception);
            EXPECT_EQ(recorded, 0);
            EXPECT_GT(drawUiFrame(*window, moved, mesh, observer), 0);
        }
        EXPECT_FALSE(observer.valid());
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, MaterialTintAndFormatRebuildMatchLinearRgbOnGpu)
    {
        Log::Registry::get("render.vulkan").setLevel(Log::Level::DEBUG);
        auto window = createWindow(false, 384, 256);
        {
            VulkanInstance instance(requiredExtensions(), ValidationMode::Required);
            GlfwSurface surface(instance, *window);
            VulkanDevice device(instance, surface);
            VulkanSwapchain swapchain(device, surface, 384, 256);
            const auto check = [&](VkFormat format, VkFormat rebuildFrom) {
                SceneAssets assets(referenceSceneAssetPaths());
                const auto pixels =
                    TextureGpuReview::capture(instance.handle(), device, *window, assets, format, {0.5f, 0.5f, 0.5f},
                                              rebuildFrom);
                const size_t white = (112 * 384 + 240) * 4, gray = (112 * 384 + 176) * 4;
                for (int c = 0; c < 3; ++c)
                {
                    EXPECT_NEAR(pixels[white + c], 188, 1);
                    EXPECT_NEAR(pixels[gray + c], 92, 1);
                    EXPECT_NEAR(pixels[(64 * 384 + 320) * 4 + c], 128, 1);
                    EXPECT_NEAR(pixels[(160 * 384 + 320) * 4 + c], 128, 1);
                }
                EXPECT_EQ(pixels[white + 3], 255);
                EXPECT_EQ(pixels[((16 + 2 * 64 + 32) * 384 + 16 + 2 * 64 + 32) * 4 + 3], 255);
                expectReleased(referenceSceneAssetPaths());
            };
            check(swapchain.format(), VK_FORMAT_UNDEFINED);
            const auto alt =
                swapchain.format() == VK_FORMAT_B8G8R8A8_UNORM ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
            check(alt, swapchain.format());
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline:"), 3u);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, InitializationFailureLeavesNextRendererUsable)
    {
        auto native = createWindow();
        DeferredFramebufferWindow window(*native);
        window.throwOnSize = true;
        EXPECT_THROW((Renderer(window, RendererValidationMode::Required)), Error::Exception);
        Renderer recovered(*native, RendererValidationMode::Required);
        submitControlledScene(*native, recovered, {}, true);
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, TextureSamplesAndUploadValidationMatchReferenceDirectionAndUi)
    {
        auto window = createWindow(false, 384, 256);
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
            auto image = VulkanImage::uploadRgba8(device, {1, 1}, VK_FORMAT_R8G8B8A8_SRGB, pixel, VK_IMAGE_USAGE_SAMPLED_BIT);
            EXPECT_EQ(image->format(), VK_FORMAT_R8G8B8A8_SRGB);
            EXPECT_NE(image->view(), VK_NULL_HANDLE);
            VulkanSwapchain swapchain(device, surface, 384, 256);
            EXPECT_EQ(swapchain.colorSpace(), VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);

            const std::array<std::array<int, 3>, 12> ref{{{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0},
                                                          {0, 255, 255}, {255, 0, 255}, {128, 128, 128}, {255, 255, 255},
                                                          {255, 128, 0}, {128, 0, 255}, {17, 34, 51}, {0, 0, 0}}};
            const std::vector<uint8_t> dir{255, 255, 255, 255, 255, 128, 0, 255, 0, 0, 0, 255, 0, 0, 255, 255,
                                           128, 128, 128, 255, 255, 0, 0, 255, 0, 255, 0, 255, 255, 0, 255, 128,
                                           0, 255, 255, 255, 17, 34, 51, 0, 255, 255, 0, 255, 128, 0, 255, 255};
            for (const bool direction : {false, true})
            {
                auto paths = referenceSceneAssetPaths();
                if (direction)
                    paths.texture = "textures/direction_rgba.png";
                SceneAssets assets(paths);
                const int cols = direction ? 3 : 4, rows = direction ? 4 : 3;
                ASSERT_EQ(assets.texture->width(), cols);
                ASSERT_EQ(assets.texture->height(), rows);
                if (direction)
                    EXPECT_EQ(assets.texture->pixels(), dir);
                const auto px = TextureGpuReview::capture(instance.handle(), device, *window, assets, swapchain.format());
                for (int r = 0; r < rows; ++r)
                    for (int c = 0; c < cols; ++c)
                    {
                        const size_t src = r * cols + c;
                        const size_t off = ((16 + (2 * r + 1) * 192 / (2 * rows)) * 384 + 16 + (2 * c + 1) * 256 / (2 * cols)) * 4;
                        for (int ch = 0; ch < 3; ++ch)
                            EXPECT_NEAR(px[off + ch], direction ? dir[src * 4 + ch] : ref[src][ch], 1);
                        EXPECT_EQ(px[off + 3], 255);
                    }
                for (const auto p : {std::array{320, 64}, std::array{320, 160}})
                    for (int ch = 0; ch < 3; ++ch)
                        EXPECT_NEAR(px[(p[1] * 384 + p[0]) * 4 + ch], 128, 1);
                EXPECT_EQ(px[(34 * 384 + 290) * 4], 255);
                EXPECT_EQ(px[(34 * 384 + 290) * 4 + 1], 0);
                EXPECT_EQ(px[(34 * 384 + 290) * 4 + 2], 255);
                expectReleased(paths);
            }
        }
        expectCleanValidation();
    }
    TEST_F(VulkanReviewTest, LambertShaderMathAndAllNormalBranchesOnGpu)
    {
        Log::Registry::get("render").setLevel(Log::Level::DEBUG);
        Log::Registry::get("render.vulkan").setLevel(Log::Level::DEBUG);
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto tintTex = renderer.createTexture(Resource::TextureResource({}, 1, 1, {128, 64, 192, 0}));
            const auto whiteTex = renderer.createTexture(Resource::TextureResource({}, 1, 1, {255, 255, 255, 255}));
            auto plane = Demo::makePlaneMesh();
            const auto zeroMesh = renderer.createMesh(plane.vertices, plane.indices);
            for (auto& vertex : plane.vertices)
                vertex.normal[2] = 1;
            const auto normMesh = renderer.createMesh(plane.vertices, plane.indices);

            const std::array<float, 3> tint{0.5f, 0.75f, 0.25f};
            MaterialDesc desc{tintTex, tint};
            EXPECT_EQ(desc.shading, ShadingMode::Unlit);
            const auto unlit = renderer.createMaterial(desc);
            desc.shading = static_cast<ShadingMode>(99);
            EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            desc.shading = ShadingMode::Lambert;
            const auto lit = renderer.createMaterial(desc);
            const auto whiteLit = renderer.createMaterial({whiteTex, {1, 1, 1}, {}, ShadingMode::Lambert});
            desc = {};
            releaseCpuAssets(assets);

            // 分支 1 & 2：线性 ambient+directional 叠加钳位，以及与 Unlit 共存。
            const auto base =
                linearFromSrgb({128 / 255.0f, 64 / 255.0f, 192 / 255.0f}) * glm::vec3(tint[0], tint[1], tint[2]);
            std::array draws{DrawItem{normMesh, lit, glm::mat4(1)}};
            auto frame = controlledFrame({384, 256}, draws);
            frame.lighting = {{0, 0, -1}, {0.3f, 0.7f, 0.5f}, 1.2f, 0.15f};
            for (const float cosine : {1.0f, 0.5f, 0.0f, -1.0f})
            {
                frame.lighting.direction = {-std::sqrt(1 - cosine * cosine), 0, -cosine};
                expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame),
                                  base * (glm::vec3(0.15f) + frame.lighting.color * 1.2f * std::max(cosine, 0.0f)));
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

            // 分支 3：零/奇异矩阵与三角内部抵消法线均退回仅输出 ambient。
            const auto clip = [](float x, float y) { return std::array<float, 3>{x / 192 - 1, y / 128 - 1, 0.5f}; };
            const auto a = clip(64.5f, 64.5f), b = clip(320.5f, 64.5f), c = clip(64.5f, 192.5f);
            const auto cancelling = renderer.createMesh(
                std::array{MeshVertex{{a[0], a[1], a[2]}, {0, 0}, {0, 0, 1}},
                           MeshVertex{{b[0], b[1], b[2]}, {0, 0}, {0, 0, -1}},
                           MeshVertex{{c[0], c[1], c[2]}, {0, 0}, {0, 0, -1}}},
                std::array<uint32_t, 3>{0, 1, 2});
            auto projective = glm::mat4(1);
            projective[0][3] = 0.25f;
            frame.lighting = {{0, 0, -1}, {1, 1, 1}, 1, 0.2f};
            draws[0] = {zeroMesh, lit, glm::mat4(1)};
            expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame), base * 0.2f);
            for (const auto& singular : {glm::scale(glm::mat4(1), glm::vec3(1, 1, 0)), projective})
            {
                draws[0] = {normMesh, lit, singular};
                expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame), base * 0.2f);
            }
            draws[0] = {cancelling, lit, glm::mat4(1)};
            FrameInput cancelFrame{{384, 256}, {glm::mat4(1), glm::mat4(1)}, draws, frame.lighting};
            for (const float sign : {-1.0f, 1.0f})
            {
                cancelFrame.lighting.direction.z = sign;
                expectLinearPixel(TextureGpuReview::captureFrame(renderer, cancelFrame), base * 0.2f, 128, 96);
            }

            // 分支 4：非单位法线量级与非均匀缩放下先归一化再光照。
            for (const float magnitude : {1.0f, 1e20f, 1e38f})
            {
                for (auto& vertex : plane.vertices)
                    vertex.normal[2] = magnitude;
                const auto bigNorm = renderer.createMesh(plane.vertices, plane.indices);
                const std::array d{DrawItem{bigNorm, whiteLit, glm::scale(glm::mat4(1), glm::vec3(1, 1, 0.001f))}};
                auto f = controlledFrame({384, 256}, d);
                f.lighting = {{0, 0, -1}, {1, 1, 1}, 1, 0};
                expectLinearPixel(TextureGpuReview::captureFrame(renderer, f), glm::vec3(1));
            }

            // 分支 5：负缩放（det < 0）翻转法线朝向，+Z 光源被遮挡而 -Z 光源受光。
            const std::array negDraws{DrawItem{normMesh, whiteLit, glm::scale(glm::mat4(1), glm::vec3(1, 1, -1))}};
            auto negFrame = controlledFrame({384, 256}, negDraws);
            negFrame.lighting = {{0, 0, -1}, {1, 1, 1}, 0.7f, 0.2f};
            expectLinearPixel(TextureGpuReview::captureFrame(renderer, negFrame), glm::vec3(0.2f));
            negFrame.lighting.direction = {0, 0, 1};
            expectLinearPixel(TextureGpuReview::captureFrame(renderer, negFrame), glm::vec3(0.9f));
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline:"), 1u);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, LambertFrameSlotsShareLightingAndRejectInvalidValuesCleanly)
    {
        Log::Registry::get("render").setLevel(Log::Level::DEBUG);
        auto native = createWindow(true, 768, 512);
        DeferredFramebufferWindow window(*native);
        window.drawable = true;
        SceneAssets assets;
        Mesh meshObserver;
        Material materialObserver;
        {
            Renderer renderer(window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            auto data = Demo::makePlaneMesh();
            for (auto& vertex : data.vertices)
                vertex.normal[2] = 1;
            meshObserver = renderer.createMesh(data.vertices, data.indices);
            materialObserver = renderer.createMaterial(
                {renderer.createTexture(Resource::TextureResource({}, 1, 1, {255, 255, 255, 0})),
                 {0.25f, 0.5f, 0.75f},
                 {},
                 ShadingMode::Lambert});
            releaseCpuAssets(assets);
            Renderer moved(std::move(renderer));
            std::array draws{
                DrawItem{meshObserver, materialObserver, glm::translate(glm::mat4(1), glm::vec3(-0.6f, 0, 0))},
                DrawItem{meshObserver, materialObserver, glm::translate(glm::mat4(1), glm::vec3(0.6f, 0, 0))}};
            for (int index = 0; index < 6; ++index)
            {
                window.pollEvents();
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
                const auto image = WindowCapture::capture(window);
                const float aspect = float(extent->width) / extent->height;
                for (const float x : {-0.6f, 0.6f})
                    expectLinearPixel(image.rgba, expected, static_cast<uint32_t>((x / aspect + 1) * image.width * 0.5f),
                                      image.height / 2, image.width);
            }
            window.throwOnSize = true;
            auto badFrame = Demo::makeSceneFrame({640, 360}, draws);
            for (const auto& invalid : {Lighting{{0, 0, 0}}, Lighting{{std::numeric_limits<float>::quiet_NaN(), 0, -1}},
                                        Lighting{{0, 0, -1}, {0, std::numeric_limits<float>::infinity(), 0}},
                                        Lighting{{0, 0, -1}, {1, 1, 1.1f}}, Lighting{{0, 0, -1}, {1, 1, 1}, -1},
                                        Lighting{{0, 0, -1}, {1, 1, 1}, 1, -0.1f},
                                        Lighting{{0, 0, -1}, {1, 1, 1}, std::numeric_limits<float>::infinity()}})
            {
                badFrame.lighting = invalid;
                bool recorded = false;
                EXPECT_THROW(moved.drawFrame(badFrame, {0, 0, 0, 1}, [&](void*) { recorded = true; }), Error::Exception);
                EXPECT_THROW(Detail::RendererReviewAccess::withFrame(
                                 moved, badFrame, [&](const auto&, const auto&, auto, auto) { recorded = true; }),
                             Error::Exception);
                EXPECT_FALSE(recorded);
            }
            window.throwOnSize = false;
            EXPECT_GT(drawUiFrame(window, moved, meshObserver, materialObserver), 0);
        }
        EXPECT_FALSE(meshObserver.valid() || materialObserver.valid());
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, FinalModelNormalMatricesAndDegenerateUnlitDrawsRemainValid)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material =
                renderer.createMaterial({renderer.createTexture(Resource::TextureResource({}, 1, 1, {255, 255, 255, 255}))});
            releaseCpuAssets(assets);
            auto data = Demo::makePlaneMesh();
            for (int i = 0; i < 3; ++i)
                data.vertices[i].normal[i] = float(i + 1);
            const auto mesh = renderer.createMesh(data.vertices, data.indices);
            const auto rot = glm::rotate(glm::mat4(1), glm::radians(37.0f), glm::normalize(glm::vec3(1, 2, 3)));
            const auto nonuniform = rot * glm::scale(glm::mat4(1), glm::vec3(2, 3, 0.5f));
            auto projective = glm::mat4(1);
            projective[0][3] = 0.25f;
            const std::array models = {glm::mat4(1),
                                       rot,
                                       nonuniform,
                                       rot * glm::scale(glm::mat4(1), glm::vec3(-2, 3, 0.5f)),
                                       rot * glm::scale(glm::mat4(1), glm::vec3(0.001f)),
                                       glm::translate(glm::mat4(1), glm::vec3(7, -2, 4)) * nonuniform,
                                       glm::scale(glm::mat4(1), glm::vec3(1, 1, 0)),
                                       projective};
            std::vector<DrawItem> draws;
            for (const auto& model : models)
                draws.push_back({mesh, material, model});
            Detail::RendererReviewAccess::withFrame(
                renderer, FrameInput{{384, 256}, {glm::mat4(1), glm::mat4(1)}, draws},
                [&](const auto& device, const auto& pipeline, auto frameSet, auto nativeDraws) {
                    ASSERT_EQ(nativeDraws.size(), models.size());
                    for (size_t i = 0; i < models.size(); ++i)
                    {
                        const auto& object = nativeDraws[i].object;
                        const auto ref = i < 6 ? glm::transpose(glm::inverse(glm::dmat3(models[i]))) : glm::dmat3(0);
                        for (int c = 0; c < 4; ++c)
                            EXPECT_EQ(object.model[c], models[i][c]);
                        for (int c = 0; c < 3; ++c)
                        {
                            EXPECT_EQ(object.normalColumns[c].w, 0);
                            for (int r = 0; r < 3; ++r)
                            {
                                EXPECT_TRUE(std::isfinite(object.normalColumns[c][r]));
                                EXPECT_NEAR(object.normalColumns[c][r], ref[c][r],
                                            0.00001 * std::max(1.0, std::abs(ref[c][r])));
                            }
                        }
                    }
                    for (int c = 0; c < 3; ++c)
                        EXPECT_EQ(nativeDraws[2].object.normalColumns[c], nativeDraws[5].object.normalColumns[c]);
                    for (size_t index : {6u, 7u})
                    {
                        const auto px =
                            TextureGpuReview::captureDraws(device, pipeline, frameSet, nativeDraws.subspan(index, 1));
                        for (int c = 0; c < 4; ++c)
                            EXPECT_EQ(px[(128 * 384 + 192) * 4 + c], 255);
                    }
                });
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, MeshCreationValidationCrossRendererRejectionAndLifecycle)
    {
        auto firstWindow = createWindow(), secondWindow = createWindow();
        SceneAssets assets;
        Mesh observer;
        {
            Renderer first(*firstWindow, RendererValidationMode::Required);
            const auto firstTex = first.createTexture(*assets.texture);
            first.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto firstMat = first.createMaterial({firstTex});
            auto data = Demo::makePlaneMesh();
            EXPECT_THROW(first.createMesh({}, data.indices), Error::Exception);
            EXPECT_THROW(first.createMesh(data.vertices, {}), Error::Exception);
            for (const auto& bad : {std::vector<uint32_t>{0, 1}, std::vector<uint32_t>{0, 1, 4},
                                    std::vector<uint32_t>{0, 1, std::numeric_limits<uint32_t>::max()}})
                EXPECT_THROW(first.createMesh(data.vertices, bad), Error::Exception);
            for (int axis = 0; axis < 3; ++axis)
                for (const float v : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
                {
                    auto badVerts = data.vertices;
                    badVerts.back().normal[axis] = v;
                    EXPECT_THROW(first.createMesh(badVerts, data.indices), Error::Exception);
                }
            auto mesh = first.createMesh(data.vertices, data.indices);
            observer = mesh;
            data = {};
            ASSERT_TRUE(mesh.valid() && observer.valid() && !Mesh{}.valid());
            {
                struct RestoreContext
                {
                    ImGuiContext* previous;
                    ~RestoreContext() { ImGui::SetCurrentContext(previous); }
                } restore{ImGui::GetCurrentContext()};
                ImGui::SetCurrentContext(nullptr);
                Renderer second(*secondWindow, RendererValidationMode::Required);
                second.prepareSceneResources(*assets.vertex, *assets.fragment);
                auto foreignGroup = first.createResources();
                EXPECT_THROW(second.createMesh(foreignGroup, data.vertices, data.indices), Error::Exception);
                EXPECT_THROW(second.createMaterial(foreignGroup, {firstTex}), Error::Exception);
                EXPECT_THROW(second.createMaterial({firstTex}), Error::Exception);
                const auto secMat = second.createMaterial({second.createTexture(*assets.texture)});
                const auto ownMesh = createCubeMesh(second);
                const auto extent = second.beginUiFrame();
                ASSERT_TRUE(extent);
                second.imgui().endFrame();
                int recorded = 0;
                for (const auto& foreign : {DrawItem{mesh, secMat, glm::mat4(1)}, DrawItem{ownMesh, firstMat, glm::mat4(1)}})
                    EXPECT_THROW(second.drawFrame(Demo::makeSceneFrame(*extent, std::array{DrawItem{ownMesh, secMat, glm::mat4(1)}, foreign}),
                                                  {0.2f, 0.3f, 0.4f, 1}, [&](void*) { ++recorded; }),
                                 Error::Exception);
                EXPECT_EQ(recorded, 0);
            }
            releaseCpuAssets(assets);
            mesh = {};
            for (int f = 0; f < 6; ++f)
                EXPECT_GT(drawUiFrame(*firstWindow, first, observer, firstMat), 0);
            Renderer moved(std::move(first));
            EXPECT_THROW(first.createMesh({}, {}), Error::Exception);
            EXPECT_GT(drawUiFrame(*firstWindow, moved, observer, firstMat), 0);
        }
        EXPECT_FALSE(observer.valid());
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, FirstZeroFramebufferVisibleMinimizeRestoreAndRapidShutdown)
    {
        auto native = createWindow(true);
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
            for (int f = 0; f < 5; ++f)
                EXPECT_FALSE(renderer.beginUiFrame());
            EXPECT_EQ(ImGui::GetFrameCount(), 0);
            EXPECT_EQ(countOccurrences(messages_.str(), "Swapchain "), skippedLogs);
            window.drawable = true;
            int recorded = 0;
            size_t steadyLogs = 0;
            for (int f = 0; f < 8; ++f)
            {
                if (prepareScene)
                    recorded += drawUiFrame(window, renderer, mesh, material);
                else
                {
                    submitControlledScene(window, renderer, {}, true);
                    ++recorded;
                }
                if (f == 0)
                    steadyLogs = countOccurrences(messages_.str(), "Swapchain ");
            }
            EXPECT_GT(recorded, 0);
            EXPECT_EQ(countOccurrences(messages_.str(), "Swapchain "), steadyLogs);
            EXPECT_TRUE(ImGui::GetIO().BackendRendererUserData && ImGui::GetIO().Fonts->IsBuilt());
            window.drawable = false;
            EXPECT_FALSE(renderer.beginUiFrame());
            EXPECT_EQ(countOccurrences(messages_.str(), "Swapchain "), steadyLogs);
        }
        FileAudit audit;
        SceneAssets assets({}, std::make_unique<AuditedFileIO>(audit));
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            const auto mesh = createCubeMesh(renderer);
            releaseCpuAssets(assets);
            auto* hwnd = static_cast<GLFWwindow*>(native->getNativeHandle());
            for (int cycle = 0; cycle < 3; ++cycle)
            {
                glfwSetWindowSize(hwnd, 680 + cycle * 20, 400 + cycle * 10);
                EXPECT_GT(drawUiFrame(*native, renderer, mesh, material), 0);
                glfwIconifyWindow(hwnd);
                for (int i = 0; i < 100 && !native->isIconified(); ++i, glfwWaitEventsTimeout(0.005))
                    native->pollEvents();
                ASSERT_TRUE(native->isIconified());
                EXPECT_EQ(drawUiFrame(*native, renderer, mesh, material), 0);
                glfwRestoreWindow(hwnd);
                for (int i = 0; i < 100 && native->isIconified(); ++i, glfwWaitEventsTimeout(0.005))
                    native->pollEvents();
                ASSERT_FALSE(native->isIconified());
                int rec = 0;
                for (int f = 0; f < 8; ++f)
                    rec += drawUiFrame(*native, renderer, mesh, material);
                EXPECT_GT(rec, 0);
                EXPECT_EQ(audit.textureOpens, 1u);
            }
        }
        expectCleanValidation();
    }
    TEST_F(VulkanReviewTest, SwapchainResizeAndMatrixProjectionControlPresentedImage)
    {
        auto window = createWindow(true, 640, 480);
        SceneAssets assets(referenceSceneAssetPaths());
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            const auto mesh = createCubeMesh(renderer);
            releaseCpuAssets(assets);
            const auto extent = renderer.beginUiFrame();
            ASSERT_TRUE(extent);
            renderer.imgui().endFrame();
            const std::array draws{DrawItem{mesh, material, glm::mat4(1)}};
            const auto stale = Demo::makeSceneFrame(*extent, draws);
            glfwSetWindowSize(static_cast<GLFWwindow*>(window->getNativeHandle()), 810, 450);
            window->pollEvents();
            int recorded = 0;
            renderer.drawFrame(stale, {0.1f, 0.2f, 0.3f, 1}, [&](void*) { ++recorded; });
            EXPECT_EQ(recorded, 0);
            const auto resizedExtent = renderer.beginUiFrame();
            ASSERT_TRUE(resizedExtent && resizedExtent->width == 810u && resizedExtent->height == 450u);
            renderer.imgui().endFrame();
            renderer.drawFrame(Demo::makeSceneFrame(*resizedExtent, draws), {0.1f, 0.2f, 0.3f, 1}, [&](void*) {
                ++recorded;
                glfwSetWindowSize(static_cast<GLFWwindow*>(window->getNativeHandle()), 640, 480);
            });
            EXPECT_EQ(recorded, 1);

            auto present = [&](float modelX, float cameraX) {
                for (int f = 0; f < 8; ++f, glfwWaitEventsTimeout(0.025))
                {
                    window->pollEvents();
                    const auto ex = renderer.beginUiFrame();
                    if (!ex)
                        throw Error::Exception(Error::Code::RENDER, "Visible matrix review frame was unavailable");
                    std::array d{DrawItem{mesh, material, glm::translate(glm::mat4(1), {modelX, 0, 0})}};
                    auto input = Demo::makeSceneFrame(*ex, d);
                    input.camera.view = glm::lookAtRH(glm::vec3(cameraX, 0, 3), glm::vec3(cameraX, 0, 0), glm::vec3(0, 1, 0));
                    renderer.imgui().endFrame();
                    renderer.drawFrame(input, {0.2f, 0.24f, 0.28f, 1});
                    d[0].model = input.camera.projection = glm::mat4(0);
                }
                return WindowCapture::capture(*window);
            };
            const auto first = present(0, 0), stable = present(0, 0);
            const auto base = WindowCapture::bounds(first);
            ASSERT_GT(base[2] - base[0], 100);
            EXPECT_NEAR(base[2] - base[0], base[3] - base[1], 2);
            size_t changed = 0;
            for (int y = 8; y < first.height - 8; ++y)
                for (int x = 8; x < first.width - 8; ++x)
                {
                    const size_t p = (size_t(y) * first.width + x) * 4;
                    changed += (std::abs(int(first.rgba[p]) - int(stable.rgba[p])) +
                                std::abs(int(first.rgba[p + 1]) - int(stable.rgba[p + 1])) +
                                std::abs(int(first.rgba[p + 2]) - int(stable.rgba[p + 2]))) > 12;
                }
            EXPECT_LT(changed, 32u);
            EXPECT_GT(WindowCapture::bounds(present(0.5f, 0))[0] - base[0], 50);
            EXPECT_GT(base[0] - WindowCapture::bounds(present(0, 0.5f))[0], 50);
            glfwSetWindowSize(static_cast<GLFWwindow*>(window->getNativeHandle()), 800, 400);
            const auto resImg = present(0, 0);
            const auto rb = WindowCapture::bounds(resImg);
            EXPECT_EQ(resImg.width, 800);
            EXPECT_EQ(resImg.height, 400);
            EXPECT_NEAR(rb[2] - rb[0], rb[3] - rb[1], 2);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, MultiObjectTransformsDepthOrderAndCountsPreserveEmptyUi)
    {
        auto native = createWindow(true, 640, 480);
        DeferredFramebufferWindow window(*native);
        window.drawable = true;
        FileAudit audit;
        SceneAssets assets(referenceSceneAssetPaths(), std::make_unique<AuditedFileIO>(audit));
        Mesh planeObs, cubeObs;
        {
            Renderer renderer(window, RendererValidationMode::Required);
            for (int f = 0; f < 4; ++f, glfwWaitEventsTimeout(0.02))
                submitControlledScene(window, renderer, {}, true);
            const auto prePrep = WindowCapture::capture(window);
            const auto px = [](const WindowCapture::Image& img, int x, int y, int c) {
                return img.rgba[(size_t(y) * img.width + x) * 4 + c];
            };
            for (int c = 0; c < 3; ++c)
                EXPECT_EQ(px(prePrep, 320, 240, c), px(prePrep, 8, 8, c));
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline"), 0u);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto texture = renderer.createTexture(*assets.texture);
            const auto mat = renderer.createMaterial({texture}), frontMat = renderer.createMaterial({texture, {0.5f, 1, 0.5f}});
            auto cube = createCubeMesh(renderer);
            const auto pd = Demo::makePlaneMesh();
            auto plane = renderer.createMesh(pd.vertices, pd.indices);
            cubeObs = cube;
            planeObs = plane;
            releaseCpuAssets(assets);
            FrameExtent lastExtent{};
            const auto present = [&](std::vector<DrawItem> draws, bool withUi = true) {
                for (int f = 0; f < 2; ++f, glfwWaitEventsTimeout(0.03))
                    lastExtent = submitControlledScene(window, renderer, draws, withUi);
                draws.clear();
                return WindowCapture::capture(window);
            };
            const auto s = glm::scale(glm::mat4(1), glm::vec3(0.55f));
            std::array<int, 4> fixed{};
            for (int f = 0; f < 6; ++f)
            {
                const float y = f % 2 == 0 ? 0.25f : -0.25f;
                const auto img = present(
                    {{plane, mat, glm::translate(glm::mat4(1), {-0.6f, 0, 0}) * s},
                     {f < 4 ? cube : plane, mat,
                      glm::translate(glm::mat4(1), {0.6f, y, 0}) * glm::rotate(glm::mat4(1), 0.4f, {0, 1, 0}) * s}},
                    false);
                const auto l = WindowCapture::bounds(img, 8, img.width / 2), r = WindowCapture::bounds(img, img.width / 2, img.width - 8);
                EXPECT_NEAR((l[0] + l[2]) * 0.5f, img.width * 0.5f - 0.6f * img.height * 0.5f, 2.0f);
                EXPECT_NEAR((r[1] + r[3]) * 0.5f, img.height * (0.5f - y * 0.5f), 2.0f);
                if (f == 0)
                    fixed = l;
                else
                    EXPECT_EQ(l, fixed);
            }
            const auto ds = glm::scale(glm::mat4(1), glm::vec3(1.4f));
            const DrawItem back{plane, mat, glm::translate(glm::mat4(1), {-0.2f, 0, -0.3f}) * ds};
            const DrawItem front{plane, frontMat, glm::translate(glm::mat4(1), {0.2f, 0, 0.3f}) * ds};
            const auto frontFirst = present({front, back}), frontLast = present({back, front});
            EXPECT_GT(px(frontFirst, frontFirst.width / 2, frontFirst.height * 3 / 10, 1), 220);
            expectInteriorEqual(frontFirst, frontLast);
            std::vector<DrawItem> grid;
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    grid.push_back({plane, mat,
                                    glm::translate(glm::mat4(1), {(x - 1.5f) * 0.5f, (1.5f - y) * 0.5f, 0}) *
                                        glm::scale(glm::mat4(1), glm::vec3(0.25f))});
            const auto many = present(grid);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                {
                    const int cx = many.width / 2 + int((x - 1.5f) * many.height / 4),
                              cy = many.height / 2 + int((y - 1.5f) * many.height / 4);
                    EXPECT_GT(std::abs(int(px(many, cx, cy, 0)) - int(px(many, 8, 8, 0))) +
                                  std::abs(int(px(many, cx, cy, 1)) - int(px(many, 8, 8, 1))) +
                                  std::abs(int(px(many, cx, cy, 2)) - int(px(many, 8, 8, 2))),
                              40);
                }
            window.drawable = false;
            renderer.drawFrame(controlledFrame(lastExtent, grid), {0.2f, 0.24f, 0.28f, 1});
            grid.clear();
            cube = plane = {};
            window.drawable = true;
            const auto empty = present({});
            for (int c = 0; c < 3; ++c)
                EXPECT_EQ(px(empty, empty.width / 2, empty.height / 2, c), px(empty, 8, 8, c));
            EXPECT_GT(px(empty, 24, 24, 0), 220);
            EXPECT_EQ(audit.textureOpens, 1u);
        }
        EXPECT_FALSE(cubeObs.valid() || planeObs.valid());
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, ValidationModesMessengerAndImGuiSingleImageRejection)
    {
        auto window = createWindow();
        {
            for (const auto mode : {ValidationMode::Optional, ValidationMode::Required, ValidationMode::Disabled})
                EXPECT_EQ(VulkanInstance(requiredExtensions(), mode).validationEnabled(), mode != ValidationMode::Disabled);
            VulkanInstance instance(requiredExtensions(), ValidationMode::Required);
            const auto submit = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
                vkGetInstanceProcAddr(instance.handle(), "vkSubmitDebugUtilsMessageEXT"));
            ASSERT_NE(submit, nullptr);
            VkDebugUtilsMessengerCallbackDataEXT data{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
            data.pMessageIdName = "INCUBATOR_REVIEW_INJECTION";
            data.pMessage = "Review injected warning";
            submit(instance.handle(), VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data);
            data.pMessage = "Review injected error with operation/VkResult evidence";
            submit(instance.handle(), VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data);

            GlfwSurface surface(instance, *window);
            VulkanDevice device(instance, surface);
            Integration::ImGuiRenderer adapter(*window);
            EXPECT_THROW(adapter.initializeVulkan(instance.handle(), device.physicalDevice(), device.handle(),
                                                  device.queueFamilies().graphics, device.graphicsQueue(),
                                                  VK_FORMAT_B8G8R8A8_UNORM, device.findDepthFormat(), 1),
                         Error::Exception);
            EXPECT_EQ(ImGui::GetIO().BackendRendererUserData, nullptr);
            adapter.initializeVulkan(instance.handle(), device.physicalDevice(), device.handle(),
                                     device.queueFamilies().graphics, device.graphicsQueue(), VK_FORMAT_B8G8R8A8_UNORM,
                                     device.findDepthFormat(), 2);
            EXPECT_TRUE(ImGui::GetIO().BackendRendererUserData && ImGui::GetIO().Fonts->IsBuilt());
        }
        EXPECT_NE(messages_.str().find("warning|Review injected warning"), std::string::npos);
        EXPECT_NE(messages_.str().find("error|Review injected error with operation/VkResult evidence"),
                  std::string::npos);
    }

    TEST_F(VulkanReviewTest, MissingLayerOptionalWarnsAndRequiredFails)
    {
        auto window = createWindow();
        EmptyLayerSearch missing;
        uint32_t count = 0;
        ASSERT_EQ(vkEnumerateInstanceLayerProperties(&count, nullptr), VK_SUCCESS);
        std::vector<VkLayerProperties> layers(count);
        ASSERT_EQ(vkEnumerateInstanceLayerProperties(&count, layers.data()), VK_SUCCESS);
        ASSERT_FALSE(std::any_of(layers.begin(), layers.end(), [](const auto& l) {
            return std::string_view(l.layerName) == "VK_LAYER_KHRONOS_validation";
        }));
        EXPECT_FALSE(VulkanInstance(requiredExtensions(), ValidationMode::Optional).validationEnabled());
        EXPECT_NE(messages_.str().find("continuing without validation"), std::string::npos);
        EXPECT_THROW(VulkanInstance(requiredExtensions(), ValidationMode::Required), Error::Exception);
        EXPECT_FALSE(VulkanInstance(requiredExtensions(), ValidationMode::Disabled).validationEnabled());
    }

    TEST_F(VulkanReviewTest, MeshTangentValidationAndNormalization)
    {
        auto window = createWindow();
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            SceneAssets assets;
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto tex = renderer.createTexture(*assets.texture);
            const auto mat = renderer.createMaterial({tex});

            const std::array<uint32_t, 3> indices{0, 1, 2};
            for (size_t component = 0; component < 4; ++component)
            {
                for (const float bad : {std::numeric_limits<float>::quiet_NaN(),
                                        std::numeric_limits<float>::infinity(),
                                        -std::numeric_limits<float>::infinity()})
                {
                    std::array<MeshVertex, 3> badVerts{{{{0, 0, 0}, {0, 0}, {0, 0, 1}, {1, 0, 0, 1}},
                                                        {{1, 0, 0}, {1, 0}, {0, 0, 1}, {1, 0, 0, 1}},
                                                        {{0, 1, 0}, {0, 1}, {0, 0, 1}, {1, 0, 0, 1}}}};
                    badVerts[1].tangent[component] = bad;
                    EXPECT_THROW(renderer.createMesh(badVerts, indices), Error::Exception);
                }
            }

            // 覆盖超大向量双精度归一化、手性符号规范化、零向量/零手性归零。
            const std::array<MeshVertex, 4> validVerts{{
                {{-1, -1, 0}, {0, 0}, {0, 0, 1}, {3e20f, 4e20f, 0.0f, -2.5f}},
                {{1, -1, 0}, {1, 0}, {0, 0, 1}, {0.0f, 5.0f, 0.0f, 0.25f}},
                {{1, 1, 0}, {1, 1}, {0, 0, 1}, {0.0f, 0.0f, 0.0f, 1.0f}},
                {{-1, 1, 0}, {0, 1}, {0, 0, 1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            }};
            const std::array<uint32_t, 6> quadIndices{0, 1, 2, 2, 3, 0};
            const auto mesh = renderer.createMesh(validVerts, quadIndices);
            std::vector<uint8_t> pixels;
            Detail::RendererReviewAccess::withFrame(
                renderer,
                FrameInput{{256, 256}, {glm::mat4(1), glm::mat4(1)}, std::array{DrawItem{mesh, mat, glm::mat4(1)}}},
                [&](const auto& d, const auto& p, auto fs, auto nd) {
                    pixels = TextureGpuReview::captureDraws(d, p, fs, nd, {0, 0, 256, 256, 0, 1});
                });
            EXPECT_FALSE(pixels.empty());
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, TextureColorSpaceAndPbrMaterialValidation)
    {
        auto window = createWindow();
        auto otherWindow = createWindow();
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            SceneAssets assets;
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);

            const Resource::TextureResource srgb1x1({}, 1, 1, {255, 255, 255, 255});
            const Resource::TextureResource linear4x4({}, 4, 4, std::vector<uint8_t>(4 * 4 * 4, 128));
            EXPECT_THROW(renderer.createTexture(srgb1x1, TextureMipMode::BaseLevel,
                                                static_cast<TextureColorSpace>(99)),
                         Error::Exception);

            const auto baseColor = renderer.createTexture(srgb1x1, TextureMipMode::BaseLevel, TextureColorSpace::Srgb);
            const auto linearBase =
                renderer.createTexture(srgb1x1, TextureMipMode::BaseLevel, TextureColorSpace::Linear);
            const auto normalFullMip =
                renderer.createTexture(linear4x4, TextureMipMode::FullChain, TextureColorSpace::Linear);
            const auto mrLinear =
                renderer.createTexture(linear4x4, TextureMipMode::FullChain, TextureColorSpace::Linear);

            // 基色纹理必须为 Srgb；法线与金属粗糙度纹理必须为 Linear。
            EXPECT_THROW(renderer.createMaterial({linearBase}), Error::Exception);
            {
                MaterialDesc desc{baseColor};
                desc.normalTexture = baseColor;
                EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            }
            {
                MaterialDesc desc{baseColor};
                desc.metallicRoughnessTexture = baseColor;
                EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            }
            // 属于其他 Renderer 或已过期的非空句柄必须被拒绝。
            {
                struct RestoreContext
                {
                    ImGuiContext* previous;
                    ~RestoreContext() { ImGui::SetCurrentContext(previous); }
                } restore{ImGui::GetCurrentContext()};
                ImGui::SetCurrentContext(nullptr);
                Renderer otherRenderer(*otherWindow, RendererValidationMode::Required);
                const auto otherLinear =
                    otherRenderer.createTexture(srgb1x1, TextureMipMode::BaseLevel, TextureColorSpace::Linear);
                MaterialDesc desc{baseColor};
                desc.normalTexture = otherLinear;
                EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            }
            {
                Texture expiredLinear;
                {
                    auto tempGroup = renderer.createResources();
                    expiredLinear = renderer.createTexture(tempGroup, srgb1x1, TextureMipMode::BaseLevel,
                                                           TextureColorSpace::Linear);
                }
                ASSERT_FALSE(expiredLinear.valid());
                MaterialDesc desc{baseColor};
                desc.metallicRoughnessTexture = expiredLinear;
                EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            }

            // 越界或非有限 PBR 参数与非法着色模式必须抛出 RENDER。
            for (const float badScale : {-0.1f, std::numeric_limits<float>::quiet_NaN(),
                                         std::numeric_limits<float>::infinity()})
            {
                MaterialDesc desc{baseColor};
                desc.shading = ShadingMode::Pbr;
                desc.normalScale = badScale;
                EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            }
            for (const float badFactor : {-0.01f, 1.01f, std::numeric_limits<float>::quiet_NaN(),
                                          std::numeric_limits<float>::infinity()})
            {
                MaterialDesc desc{baseColor};
                desc.shading = ShadingMode::Pbr;
                desc.metallic = badFactor;
                EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
                desc.metallic = 0.5f;
                desc.roughness = badFactor;
                EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            }
            {
                MaterialDesc desc{baseColor};
                desc.shading = static_cast<ShadingMode>(99);
                EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            }

            // 资源组持有期：材质强持有 normalTexture 与 metallicRoughnessTexture，释放纹理组后仍存活。
            {
                auto texGroup = renderer.createResources();
                auto matGroup = renderer.createResources();
                const auto scopedNormal =
                    renderer.createTexture(texGroup, linear4x4, TextureMipMode::FullChain, TextureColorSpace::Linear);
                const auto scopedMr =
                    renderer.createTexture(texGroup, linear4x4, TextureMipMode::BaseLevel, TextureColorSpace::Linear);
                MaterialDesc desc{baseColor};
                desc.shading = ShadingMode::Pbr;
                desc.normalTexture = scopedNormal;
                desc.metallicRoughnessTexture = scopedMr;
                desc.normalScale = 1.0f;
                desc.metallic = 0.2f;
                desc.roughness = 0.6f;
                const auto scopedMat = renderer.createMaterial(matGroup, desc);
                texGroup.reset();
                EXPECT_TRUE(scopedNormal.valid());
                EXPECT_TRUE(scopedMr.valid());
                EXPECT_EQ(Detail::RendererReviewAccess::resourceCounts(renderer).textures, 6u);
                matGroup.reset();
                EXPECT_FALSE(scopedMat.valid());
                EXPECT_FALSE(scopedNormal.valid());
                EXPECT_FALSE(scopedMr.valid());
                EXPECT_EQ(Detail::RendererReviewAccess::resourceCounts(renderer).textures, 4u);
            }

            // Required Validation 下分别使用共享回退贴图与实际 FullChain 数据贴图完成绘制（含 1x1 基色 + FullChain 法线）。
            MaterialDesc fallbackPbr{baseColor};
            fallbackPbr.shading = ShadingMode::Pbr;
            fallbackPbr.sampler = {TextureFilter::Linear, TextureFilter::Linear, MipFilter::Linear};
            const auto matFallback = renderer.createMaterial(fallbackPbr);

            MaterialDesc fullPbr{baseColor};
            fullPbr.shading = ShadingMode::Pbr;
            fullPbr.sampler = {TextureFilter::Linear, TextureFilter::Linear, MipFilter::Linear};
            fullPbr.normalTexture = normalFullMip;
            fullPbr.metallicRoughnessTexture = mrLinear;
            fullPbr.normalScale = 1.0f;
            fullPbr.metallic = 0.8f;
            fullPbr.roughness = 0.3f;
            const auto matFull = renderer.createMaterial(fullPbr);

            const auto quad = Demo::makePlaneMesh();
            const auto mesh = renderer.createMesh(quad.vertices, quad.indices);
            const std::array draws{
                DrawItem{mesh, matFallback, glm::translate(glm::mat4(1), glm::vec3(-0.5f, 0, 0))},
                DrawItem{mesh, matFull, glm::translate(glm::mat4(1), glm::vec3(0.5f, 0, 0))},
            };
            std::vector<uint8_t> pixels;
            Detail::RendererReviewAccess::withFrame(
                renderer, FrameInput{{256, 256}, {glm::mat4(1), glm::mat4(1)}, draws},
                [&](const auto& d, const auto& p, auto fs, auto nd) {
                    pixels = TextureGpuReview::captureDraws(d, p, fs, nd, {0, 0, 256, 256, 0, 1});
                });
            EXPECT_FALSE(pixels.empty());
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, PbrShadingNormalPerturbationGgxAndMirroredTbn)
    {
        auto window = createWindow();
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            SceneAssets assets;
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);

            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const auto baseWhite = renderer.createTexture(whiteTex, TextureMipMode::BaseLevel, TextureColorSpace::Srgb);

            // +Y 切线空间法线扰动：nTs ≈ (0, 0.8, 0.6) -> RGBA (128, 230, 204, 255)。
            const Resource::TextureResource plusYNormalTex({}, 1, 1, {128, 230, 204, 255});
            const auto plusYNormal =
                renderer.createTexture(plusYNormalTex, TextureMipMode::BaseLevel, TextureColorSpace::Linear);

            auto projection = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 10.0f);
            projection[1][1] *= -1.0f;
            const CameraMatrices camera{
                glm::lookAtRH(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                projection};

            const std::array<uint32_t, 6> indices{0, 1, 2, 2, 3, 0};
            const std::array<MeshVertex, 4> tangentQuad{{
                {{-1, -1, 0}, {0, 0}, {0, 0, 1}, {1, 0, 0, 1}},
                {{1, -1, 0}, {1, 0}, {0, 0, 1}, {1, 0, 0, 1}},
                {{1, 1, 0}, {1, 1}, {0, 0, 1}, {1, 0, 0, 1}},
                {{-1, 1, 0}, {0, 1}, {0, 0, 1}, {1, 0, 0, 1}},
            }};
            const std::array<MeshVertex, 4> zeroTangentQuad{{
                {{-1, -1, 0}, {0, 0}, {0, 0, 1}, {0, 0, 0, 0}},
                {{1, -1, 0}, {1, 0}, {0, 0, 1}, {0, 0, 0, 0}},
                {{1, 1, 0}, {1, 1}, {0, 0, 1}, {0, 0, 0, 0}},
                {{-1, 1, 0}, {0, 1}, {0, 0, 1}, {0, 0, 0, 0}},
            }};
            const auto mesh = renderer.createMesh(tangentQuad, indices);
            const auto zeroTanMesh = renderer.createMesh(zeroTangentQuad, indices);

            const auto captureCenter = [&](const Mesh& m, const Material& mat, const glm::mat4& model,
                                           const Lighting& lighting) {
                std::vector<uint8_t> pixels;
                Detail::RendererReviewAccess::withFrame(
                    renderer,
                    FrameInput{{384, 256}, camera, std::array{DrawItem{m, mat, model}}, lighting},
                    [&](const auto& d, const auto& p, auto fs, auto nd) {
                        pixels = TextureGpuReview::captureDraws(d, p, fs, nd, {0, 0, 384, 256, 0, 1});
                    });
                return pixels;
            };

            // 1. 固定光照参数验证 Cook-Torrance GGX 与能量守恒漫反射参考值（0.05 + 0.384 = 0.434）。
            {
                MaterialDesc desc{baseWhite, {0.5f, 0.5f, 0.5f}};
                desc.shading = ShadingMode::Pbr;
                desc.metallic = 0.0f;
                desc.roughness = 0.5f;
                const auto mat = renderer.createMaterial(desc);
                const auto pixels = captureCenter(mesh, mat, glm::mat4(1.0f),
                                                  {{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 0.6f, 0.1f});
                expectLinearPixel(pixels, glm::vec3(0.434f));
            }

            // 2. +Y 切线空间法线贴图：正缩放受光更亮、单轴负缩放镜像模型反转副切线而变暗；
            //    normalScale = 0 或零切线安全退化为几何法线。
            {
                const Lighting angledLight{{0.0f, -0.8f, -0.6f}, {1.0f, 1.0f, 1.0f}, 0.8f, 0.1f};
                MaterialDesc flatDesc{baseWhite, {0.8f, 0.8f, 0.8f}};
                flatDesc.shading = ShadingMode::Pbr;
                flatDesc.metallic = 0.0f;
                flatDesc.roughness = 0.6f;
                const auto flatMat = renderer.createMaterial(flatDesc);

                MaterialDesc perturbedDesc = flatDesc;
                perturbedDesc.normalTexture = plusYNormal;
                perturbedDesc.normalScale = 1.0f;
                const auto perturbedMat = renderer.createMaterial(perturbedDesc);

                MaterialDesc zeroScaleDesc = perturbedDesc;
                zeroScaleDesc.normalScale = 0.0f;
                const auto zeroScaleMat = renderer.createMaterial(zeroScaleDesc);

                const auto flatPx = captureCenter(mesh, flatMat, glm::mat4(1.0f), angledLight);
                const auto perturbedPx = captureCenter(mesh, perturbedMat, glm::mat4(1.0f), angledLight);
                const auto mirroredPx = captureCenter(
                    mesh, perturbedMat, glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -1.0f, 1.0f)), angledLight);
                const auto zeroScalePx = captureCenter(mesh, zeroScaleMat, glm::mat4(1.0f), angledLight);
                const auto zeroTanPx = captureCenter(zeroTanMesh, perturbedMat, glm::mat4(1.0f), angledLight);

                const size_t centerOff = (128u * 384u + 192u) * 4u;
                EXPECT_GT(perturbedPx[centerOff], flatPx[centerOff] + 20);
                EXPECT_LT(mirroredPx[centerOff] + 40, flatPx[centerOff]);
                EXPECT_NEAR(zeroScalePx[centerOff], flatPx[centerOff], 1);
                EXPECT_NEAR(zeroTanPx[centerOff], flatPx[centerOff], 1);
            }

            // 3. 低粗糙度 vs 高粗糙度 GGX 高光峰值差异，以及纯金属环境光/漫反射衰减。
            {
                const Lighting frontLight{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 0.5f, 0.05f};
                MaterialDesc smoothDesc{baseWhite, {0.5f, 0.5f, 0.5f}};
                smoothDesc.shading = ShadingMode::Pbr;
                smoothDesc.metallic = 0.0f;
                smoothDesc.roughness = 0.1f;
                MaterialDesc roughDesc = smoothDesc;
                roughDesc.roughness = 0.9f;
                const auto smoothPx = captureCenter(mesh, renderer.createMaterial(smoothDesc), glm::mat4(1.0f), frontLight);
                const auto roughPx = captureCenter(mesh, renderer.createMaterial(roughDesc), glm::mat4(1.0f), frontLight);
                const size_t centerOff = (128u * 384u + 192u) * 4u;
                EXPECT_GT(smoothPx[centerOff], roughPx[centerOff] + 50);

                MaterialDesc metalDesc{baseWhite, {0.8f, 0.8f, 0.8f}};
                metalDesc.shading = ShadingMode::Pbr;
                metalDesc.metallic = 1.0f;
                metalDesc.roughness = 0.5f;
                const Lighting backLight{{0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, 1.0f, 0.3f};
                const auto metalBackPx = captureCenter(mesh, renderer.createMaterial(metalDesc), glm::mat4(1.0f), backLight);
                expectLinearPixel(metalBackPx, glm::vec3(0.0f));
            }
        }
        expectCleanValidation();
    }
}  // namespace
