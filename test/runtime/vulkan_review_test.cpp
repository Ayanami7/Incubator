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
#include <glm/gtc/packing.hpp>
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
#include "runtime/model/model_manager.h"
#include "runtime/render/detail/renderer_review.h"
#include "runtime/render/integration/imgui_renderer.h"
#include "runtime/render/renderer.h"
#include "runtime/render/vulkan/glfw_surface.h"
#include "runtime/render/vulkan/vulkan_device.h"
#include "runtime/render/vulkan/vulkan_frame_scheduler.h"
#include "runtime/render/vulkan/vulkan_image.h"
#include "runtime/render/vulkan/vulkan_instance.h"
#include "runtime/render/vulkan/vulkan_swapchain.h"
#include "runtime/resource/loader/float_texture_resource.h"
#include "runtime/resource/loader/model_loader.h"
#include "runtime/resource/loader/shader_loader.h"
#include "runtime/resource/loader/shader_resource.h"
#include "runtime/resource/loader/texture_loader.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"
#include "runtime/scene/scene.h"
#include "runtime/scene/scene_file.h"
#include "runtime/scene/scene_frame.h"
#include "runtime/scene/scene_rebuild.h"
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
        return GlfwSurface::requiredInstanceExtensions();
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

        Mesh createLitPlane(Renderer& renderer)
        {
            auto plane = Demo::makePlaneMesh();
            for (auto& vertex : plane.vertices)
                vertex.normal[2] = 1.0f;
            return renderer.createMesh(plane.vertices, plane.indices);
        }

        std::vector<std::array<float, 4>> captureHdr(Renderer& renderer, const FrameInput& input)
        {
            std::vector<std::array<float, 4>> pixels;
            Detail::RendererReviewAccess::withHdrSceneTarget(
                renderer, input, {0, 0, 0, 0}, [&](const auto& device, const auto& image) {
                    pixels = TextureGpuReview::captureRgba16f(device, image, {input.extent.width, input.extent.height});
                });
            return pixels;
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

    TEST_F(VulkanReviewTest, AlphaMaskCutsMainAndShadowPixelsAtTheSameThreshold)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            auto& manager = Resource::ResourceManager::instance();
            const auto vertex = manager.hold<Resource::ShaderResource>(
                Resource::ResourceType::SHADER, "shaders/shadow.vert.spv");
            const auto maskVertex = manager.hold<Resource::ShaderResource>(
                Resource::ResourceType::SHADER, "shaders/shadow_mask.vert.spv");
            const auto maskFragment = manager.hold<Resource::ShaderResource>(
                Resource::ResourceType::SHADER, "shaders/shadow_mask.frag.spv");
            ASSERT_TRUE(vertex && maskVertex && maskFragment);
            renderer.prepareShadowResources(*vertex.get<Resource::ShaderResource>(),
                                            *maskVertex.get<Resource::ShaderResource>(),
                                            *maskFragment.get<Resource::ShaderResource>());
            const auto plane = createLitPlane(renderer);
            const Resource::TextureResource alphaTex({}, 2, 1, {255, 0, 0, 127, 255, 0, 0, 255});
            MaterialDesc mask{renderer.createTexture(alphaTex)};
            mask.alphaMode = AlphaMode::Mask;
            mask.alphaCutoff = 0.5f;
            mask.baseAlpha = 0.5f;
            std::array caster{DrawItem{plane, renderer.createMaterial(mask), glm::translate(glm::mat4(1), {0, 0, 0.3f})}};
            constexpr FrameExtent extent{128, 128};
            const auto left = size_t(64) * extent.width + 48;
            const auto right = size_t(64) * extent.width + 80;
            const auto main = captureHdr(renderer, controlledFrame(extent, caster));
            ASSERT_EQ(main.size(), size_t(extent.width) * extent.height);
            EXPECT_EQ(main[left], (std::array<float, 4>{0, 0, 0, 0}));
            EXPECT_EQ(main[right], (std::array<float, 4>{1, 0, 0, 1}));

            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            MaterialDesc white{renderer.createTexture(whiteTex)};
            white.shading = ShadingMode::Lambert;
            const std::array receiver{DrawItem{plane, renderer.createMaterial(white), glm::mat4(1)}};
            auto frame = controlledFrame(extent, receiver);
            frame.lighting = {{0, 0, -1}, {1, 1, 1}, 1.0f, 0.0f};
            ShadowSettings settings;
            settings.enabled = true;
            frame.shadow = makeShadowFrame(settings, frame.lighting.direction);
            frame.shadowDraws = caster;
            const auto shadow = captureHdr(renderer, frame);
            ASSERT_EQ(shadow.size(), main.size());
            EXPECT_NEAR(shadow[left][0], 1.0f, 0.005f);
            EXPECT_NEAR(shadow[right][0], 0.0f, 0.005f);
            mask.alphaMode = AlphaMode::Blend;
            caster[0].material = renderer.createMaterial(mask);
            const auto withoutBlendShadow = captureHdr(renderer, frame);
            ASSERT_EQ(withoutBlendShadow.size(), main.size());
            EXPECT_NEAR(withoutBlendShadow[right][0], 1.0f, 0.005f);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, DoubleSidedLightsBackFacesAndSingleSidedCullsThem)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto plane = createLitPlane(renderer);
            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const auto white = renderer.createTexture(whiteTex);
            constexpr FrameExtent extent{64, 64};
            constexpr size_t center = (extent.height / 2) * extent.width + extent.width / 2;
            for (const auto shading : {ShadingMode::Lambert, ShadingMode::Pbr})
            {
                SCOPED_TRACE(static_cast<int>(shading));
                MaterialDesc desc{white, {0.6f, 0.6f, 0.6f}};
                desc.shading = shading;
                desc.doubleSided = true;
                std::array draws{DrawItem{plane, renderer.createMaterial(desc), glm::mat4(1)}};
                auto frame = controlledFrame(extent, draws);
                frame.lighting = {{0, 0, -1}, {1, 1, 1}, 1.0f, 0.0f};
                const auto front = captureHdr(renderer, frame);
                frame.camera.view = glm::lookAtRH(glm::vec3(0, 0, -3), glm::vec3(0), glm::vec3(0, 1, 0));
                frame.lighting.direction = {0, 0, 1};
                const auto back = captureHdr(renderer, frame);
                ASSERT_GT(front.size(), center);
                ASSERT_GT(back.size(), center);
                EXPECT_GT(front[center][0], 0.2f);
                EXPECT_NEAR(back[center][0], front[center][0], 0.005f);
                EXPECT_EQ(back[center][3], 1.0f);
                desc.doubleSided = false;
                draws[0].material = renderer.createMaterial(desc);
                const auto culled = captureHdr(renderer, frame);
                ASSERT_GT(culled.size(), center);
                EXPECT_EQ(culled[center], (std::array<float, 4>{0, 0, 0, 0}));
            }
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, EmissiveTextureContributesInDarknessAndPassesThroughToneMapping)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);
            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const Resource::TextureResource glowTex({}, 1, 1, {255, 128, 64, 255});
            const auto white = renderer.createTexture(whiteTex);
            const auto glow = renderer.createTexture(glowTex);
            const auto plane = createLitPlane(renderer);
            constexpr FrameExtent extent{64, 64};
            constexpr size_t center = (extent.height / 2) * extent.width + extent.width / 2;
            const auto expected = linearFromSrgb(glm::vec3(255, 128, 64) / 255.0f) * glm::vec3(0.5f, 1.0f, 0.75f);
            for (const auto shading : {ShadingMode::Lambert, ShadingMode::Pbr})
            {
                SCOPED_TRACE(static_cast<int>(shading));
                MaterialDesc desc{white};
                desc.shading = shading;
                desc.emissiveTexture = glow;
                desc.emissiveFactor = {0.5f, 1.0f, 0.75f};
                const std::array draws{DrawItem{plane, renderer.createMaterial(desc), glm::mat4(1)}};
                auto frame = controlledFrame(extent, draws);
                frame.lighting = {{0, 0, -1}, {1, 1, 1}, 0.0f, 0.0f};
                frame.display = {ToneMappingMode::Reinhard, 1.0f};
                const auto hdr = captureHdr(renderer, frame);
                ASSERT_GT(hdr.size(), center);
                for (int channel = 0; channel < 3; ++channel)
                    EXPECT_NEAR(hdr[center][channel], expected[channel], 0.003f);
                EXPECT_EQ(hdr[center][3], 1.0f);
                ASSERT_TRUE(renderer.beginUiFrame());
                const auto output = renderer.prepareSceneTarget(extent);
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(output, frame, {0, 0, 0, 1});
                Detail::RendererReviewAccess::withSceneOutput(renderer, output, [&](const auto& device, const auto& image) {
                    const auto ldr = TextureGpuReview::captureMip(device, image, 0, {extent.width, extent.height});
                    const auto exposed = expected * 2.0f;
                    expectLinearPixel(ldr, exposed / (glm::vec3(1) + exposed), 32, 32, extent.width);
                });
            }
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, AlphaBlendCompositesOverBackgroundAndOpaqueWithoutDepthWrite)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);
            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const auto white = renderer.createTexture(whiteTex);
            const auto plane = createLitPlane(renderer);

            auto makeUnlit = [&](std::array<float, 3> color, AlphaMode mode, float alpha = 1.0f) {
                MaterialDesc desc{white, color};
                desc.shading = ShadingMode::Unlit;
                desc.alphaMode = mode;
                desc.baseAlpha = alpha;
                desc.doubleSided = true;
                return renderer.createMaterial(desc);
            };

            const auto blendRed = makeUnlit({1.0f, 0.0f, 0.0f}, AlphaMode::Blend, 0.5f);
            const auto blendBlue = makeUnlit({0.0f, 0.0f, 1.0f}, AlphaMode::Blend, 0.5f);
            const auto opaqueGreen = makeUnlit({0.0f, 1.0f, 0.0f}, AlphaMode::Opaque);

            // 1. 纯背景上的半透明混合：验证离屏 HDR 路径（避免双重乘 Alpha/压暗）与直接呈现路径一致。
            {
                const std::array draws{DrawItem{plane, blendRed, glm::mat4(1.0f)}};
                const auto direct = TextureGpuReview::captureFrame(renderer, controlledFrame({384, 256}, draws));
                const size_t directCenter = (128u * 384u + 192u) * 4u;
                ASSERT_GT(direct.size(), directCenter + 3u);
                EXPECT_NEAR(direct[directCenter + 0], 192, 2);
                EXPECT_NEAR(direct[directCenter + 1], 64, 2);
                EXPECT_NEAR(direct[directCenter + 2], 64, 2);
                EXPECT_EQ(direct[directCenter + 3], 255);

                constexpr FrameExtent offscreenExtent{64, 64};
                auto offscreen = controlledFrame(offscreenExtent, draws);
                offscreen.display = {ToneMappingMode::Off, 0.0f};
                const auto hdr = captureHdr(renderer, offscreen);
                constexpr size_t hdrCenter = 32u * 64u + 32u;
                ASSERT_GT(hdr.size(), hdrCenter);
                EXPECT_NEAR(hdr[hdrCenter][0], 0.5f, 0.005f);
                EXPECT_NEAR(hdr[hdrCenter][1], 0.0f, 0.005f);
                EXPECT_NEAR(hdr[hdrCenter][2], 0.0f, 0.005f);
                EXPECT_NEAR(hdr[hdrCenter][3], 0.5f, 0.005f);

                ASSERT_TRUE(renderer.beginUiFrame());
                const auto output = renderer.prepareSceneTarget(offscreenExtent);
                renderer.imgui().endFrame();
                const float midGray = 128.0f / 255.0f;
                renderer.drawSceneTarget(output, offscreen, {midGray, midGray, midGray, 1.0f});
                Detail::RendererReviewAccess::withSceneOutput(renderer, output, [&](const auto& device, const auto& image) {
                    const auto ldr = TextureGpuReview::captureMip(device, image, 0, {64, 64});
                    const size_t offCenter = hdrCenter * 4u;
                    ASSERT_GT(ldr.size(), offCenter + 3u);
                    EXPECT_NEAR(ldr[offCenter + 0], 192, 2);
                    EXPECT_NEAR(ldr[offCenter + 1], 64, 2);
                    EXPECT_NEAR(ldr[offCenter + 2], 64, 2);
                    EXPECT_EQ(ldr[offCenter + 3], 255);
                });
            }

            // 2. 不透明背景之上的半透明混合（HDR 颜色与 LDR sRGB 输出）。
            {
                constexpr FrameExtent extent{64, 64};
                const std::array draws{
                    DrawItem{plane, opaqueGreen, glm::mat4(1.0f)},
                    DrawItem{plane, blendBlue, glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, 0.2f})},
                };
                auto frame = controlledFrame(extent, draws);
                frame.display = {ToneMappingMode::Off, 0.0f};
                const auto hdr = captureHdr(renderer, frame);
                constexpr size_t center = 32u * 64u + 32u;
                ASSERT_GT(hdr.size(), center);
                EXPECT_NEAR(hdr[center][0], 0.0f, 0.005f);
                EXPECT_NEAR(hdr[center][1], 0.5f, 0.005f);
                EXPECT_NEAR(hdr[center][2], 0.5f, 0.005f);
                EXPECT_EQ(hdr[center][3], 1.0f);

                ASSERT_TRUE(renderer.beginUiFrame());
                const auto output = renderer.prepareSceneTarget(extent);
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(output, frame, {0.0f, 0.0f, 0.0f, 1.0f});
                Detail::RendererReviewAccess::withSceneOutput(renderer, output, [&](const auto& device, const auto& image) {
                    const auto ldr = TextureGpuReview::captureMip(device, image, 0, {64, 64});
                    expectLinearPixel(ldr, glm::vec3(0.0f, 0.5f, 0.5f), 32, 32, extent.width);
                });
            }

            // 3. 验证半透明不写入深度：先画近处 Blend，再画较远 Opaque 仍可写入；先画近处 Opaque 则遮挡较远 Blend。
            {
                constexpr FrameExtent extent{64, 64};
                constexpr size_t center = 32u * 64u + 32u;
                const std::array blendFirst{
                    DrawItem{plane, blendRed, glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, 0.5f})},
                    DrawItem{plane, opaqueGreen, glm::mat4(1.0f)},
                };
                const auto hdrAfterOpaque = captureHdr(renderer, controlledFrame(extent, blendFirst));
                ASSERT_GT(hdrAfterOpaque.size(), center);
                EXPECT_NEAR(hdrAfterOpaque[center][0], 0.0f, 0.005f);
                EXPECT_NEAR(hdrAfterOpaque[center][1], 1.0f, 0.005f);
                EXPECT_NEAR(hdrAfterOpaque[center][2], 0.0f, 0.005f);
                EXPECT_EQ(hdrAfterOpaque[center][3], 1.0f);

                const std::array opaqueFront{
                    DrawItem{plane, opaqueGreen, glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, 0.5f})},
                    DrawItem{plane, blendRed, glm::mat4(1.0f)},
                };
                const auto hdrOccluded = captureHdr(renderer, controlledFrame(extent, opaqueFront));
                ASSERT_GT(hdrOccluded.size(), center);
                EXPECT_NEAR(hdrOccluded[center][0], 0.0f, 0.005f);
                EXPECT_NEAR(hdrOccluded[center][1], 1.0f, 0.005f);
                EXPECT_NEAR(hdrOccluded[center][2], 0.0f, 0.005f);
                EXPECT_EQ(hdrOccluded[center][3], 1.0f);
            }
        }
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
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline"), 4u);
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
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline:"), 4u);
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
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline"), 4u);
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
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);
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
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);
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

            // 分支 5：从镜像后的正面观察，负 Z 缩放使 -Z 侧成为正面并使法线指向 -Z。
            const std::array negDraws{DrawItem{normMesh, whiteLit, glm::scale(glm::mat4(1), glm::vec3(1, 1, -1))}};
            auto negFrame = controlledFrame({384, 256}, negDraws);
            negFrame.camera.view = glm::lookAtRH(glm::vec3(0, 0, -3), glm::vec3(0), glm::vec3(0, 1, 0));
            negFrame.lighting = {{0, 0, -1}, {1, 1, 1}, 0.7f, 0.2f};
            expectLinearPixel(TextureGpuReview::captureFrame(renderer, negFrame), glm::vec3(0.2f));
            negFrame.lighting.direction = {0, 0, 1};
            expectLinearPixel(TextureGpuReview::captureFrame(renderer, negFrame), glm::vec3(0.9f));
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline:"), 4u);
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
            for (int f = 0; f < 4; ++f, glfwWaitEventsTimeout(0.02))
                submitControlledScene(window, moved, {}, true);
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

    TEST_F(VulkanReviewTest, LinearHdrSceneTargetPreservesSuperBrightValuesAndDepth)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const auto white = renderer.createTexture(whiteTex, TextureMipMode::BaseLevel, TextureColorSpace::Srgb);
            const std::array<MeshVertex, 4> quadVertices{
                MeshVertex{{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
                MeshVertex{{0.5f, -0.5f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
                MeshVertex{{0.5f, 0.5f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
                MeshVertex{{-0.5f, 0.5f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}}};
            const std::array<uint32_t, 6> quadIndices{0, 1, 2, 0, 2, 3};
            const auto quad = renderer.createMesh(quadVertices, quadIndices);

            MaterialDesc leftDesc{white, {0.5f, 0.4f, 0.35f}};
            leftDesc.shading = ShadingMode::Lambert;
            const auto leftMat = renderer.createMaterial(leftDesc);

            MaterialDesc rightDesc{white, {1.0f, 0.8f, 0.5f}};
            rightDesc.shading = ShadingMode::Lambert;
            const auto rightMat = renderer.createMaterial(rightDesc);

            MaterialDesc occluderDesc{white, {0.2f, 0.2f, 0.2f}};
            occluderDesc.shading = ShadingMode::Unlit;
            const auto occluderMat = renderer.createMaterial(occluderDesc);

            const Lighting brightLight{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 4.0f, 0.0f};
            constexpr FrameExtent EXTENT{128, 64};
            const std::array<float, 4> clearColor{0.1f, 0.2f, 0.3f, 0.0f};

            // 1. 两个超亮样本在 VK_FORMAT_R16G16B16A16_SFLOAT 中保留各自大于 1.0 的线性值，背景 alpha 为 0。
            const std::array draws{
                DrawItem{quad, leftMat, glm::translate(glm::mat4(1.0f), {-0.5f, 0.0f, 0.5f}) *
                                            glm::scale(glm::mat4(1.0f), {0.4f, 0.6f, 1.0f})},
                DrawItem{quad, rightMat, glm::translate(glm::mat4(1.0f), {0.5f, 0.0f, 0.5f}) *
                                             glm::scale(glm::mat4(1.0f), {0.4f, 0.6f, 1.0f})}};
            const FrameInput hdrInput{EXTENT, {glm::mat4(1.0f), glm::mat4(1.0f)}, draws, brightLight};
            std::vector<std::array<float, 4>> hdrPixels;
            Detail::RendererReviewAccess::withHdrSceneTarget(
                renderer, hdrInput, clearColor, [&](const auto& device, const auto& image) {
                    EXPECT_EQ(image.format(), VK_FORMAT_R16G16B16A16_SFLOAT);
                    hdrPixels = TextureGpuReview::captureRgba16f(device, image, {EXTENT.width, EXTENT.height});
                });
            ASSERT_EQ(hdrPixels.size(), size_t(EXTENT.width) * EXTENT.height);

            const auto& leftPx = hdrPixels[size_t(32) * EXTENT.width + 32];
            const auto& rightPx = hdrPixels[size_t(32) * EXTENT.width + 96];
            const auto& bgPx = hdrPixels[size_t(32) * EXTENT.width + 64];

            for (int c = 0; c < 3; ++c)
            {
                EXPECT_GT(leftPx[c], 1.0f);
                EXPECT_GT(rightPx[c], leftPx[c] + 0.5f);
            }
            EXPECT_NEAR(leftPx[0], 2.0f, 0.01f);
            EXPECT_NEAR(leftPx[1], 1.6f, 0.01f);
            EXPECT_NEAR(leftPx[2], 1.4f, 0.01f);
            EXPECT_NEAR(leftPx[3], 1.0f, 0.001f);

            EXPECT_NEAR(rightPx[0], 4.0f, 0.01f);
            EXPECT_NEAR(rightPx[1], 3.2f, 0.01f);
            EXPECT_NEAR(rightPx[2], 2.0f, 0.01f);
            EXPECT_NEAR(rightPx[3], 1.0f, 0.001f);

            EXPECT_NEAR(bgPx[0], clearColor[0], 0.005f);
            EXPECT_NEAR(bgPx[1], clearColor[1], 0.005f);
            EXPECT_NEAR(bgPx[2], clearColor[2], 0.005f);
            EXPECT_NEAR(bgPx[3], 0.0f, 0.001f);

            // 2. 验证 HDR 场景目标深度测试生效（更近的 Unlit 遮挡物覆盖后方超亮方块）。
            const std::array depthDraws{
                DrawItem{quad, rightMat, glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, 0.8f}) *
                                             glm::scale(glm::mat4(1.0f), {0.6f, 0.6f, 1.0f})},
                DrawItem{quad, occluderMat, glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, 0.2f}) *
                                                glm::scale(glm::mat4(1.0f), {0.4f, 0.4f, 1.0f})}};
            Detail::RendererReviewAccess::withHdrSceneTarget(
                renderer, FrameInput{EXTENT, {glm::mat4(1.0f), glm::mat4(1.0f)}, depthDraws, brightLight}, clearColor,
                [&](const auto& device, const auto& image) {
                    hdrPixels = TextureGpuReview::captureRgba16f(device, image, {EXTENT.width, EXTENT.height});
                });
            const auto& occludedPx = hdrPixels[size_t(32) * EXTENT.width + 64];
            EXPECT_NEAR(occludedPx[0], 0.2f, 0.01f);
            EXPECT_NEAR(occludedPx[1], 0.2f, 0.01f);
            EXPECT_NEAR(occludedPx[2], 0.2f, 0.01f);
            EXPECT_NEAR(occludedPx[3], 1.0f, 0.001f);

            // 3. 验证同 Renderer 下现有 drawFrame LDR 路径不受影响（含 Unlit/Lambert/Pbr 三种模式）。
            auto orthoProj = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 10.0f);
            orthoProj[1][1] *= -1.0f;
            const CameraMatrices pbrCamera{
                glm::lookAtRH(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)), orthoProj};
            MaterialDesc pbrDesc{white, {0.5f, 0.5f, 0.5f}};
            pbrDesc.shading = ShadingMode::Pbr;
            pbrDesc.metallic = 0.0f;
            pbrDesc.roughness = 0.5f;
            const auto pbrMat = renderer.createMaterial(pbrDesc);
            const std::array pbrDraw{DrawItem{quad, pbrMat, glm::mat4(1.0f)}};
            Detail::RendererReviewAccess::withFrame(
                renderer,
                FrameInput{{384, 256}, pbrCamera, pbrDraw, {{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 0.6f, 0.1f}},
                [&](const auto& device, const auto& pipeline, auto frameSet, auto nativeDraws) {
                    const auto ldrPx = TextureGpuReview::captureDraws(device, pipeline, frameSet, nativeDraws);
                    expectLinearPixel(ldrPx, glm::vec3(0.434f));
                });
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, DisplayPassToneMappingExposureBackgroundAndSlotResizeValidation)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);

            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const auto white = renderer.createTexture(whiteTex, TextureMipMode::BaseLevel, TextureColorSpace::Srgb);
            const std::array<MeshVertex, 4> quadVertices{
                MeshVertex{{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
                MeshVertex{{0.5f, -0.5f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
                MeshVertex{{0.5f, 0.5f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
                MeshVertex{{-0.5f, 0.5f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}}};
            const std::array<uint32_t, 6> quadIndices{0, 1, 2, 0, 2, 3};
            const auto quad = renderer.createMesh(quadVertices, quadIndices);

            MaterialDesc leftDesc{white, {0.5f, 0.25f, 0.1f}};
            leftDesc.shading = ShadingMode::Lambert;
            const auto leftMat = renderer.createMaterial(leftDesc);

            MaterialDesc rightDesc{white, {1.0f, 0.8f, 0.5f}};
            rightDesc.shading = ShadingMode::Lambert;
            const auto rightMat = renderer.createMaterial(rightDesc);

            const std::array draws{
                DrawItem{quad, leftMat, glm::translate(glm::mat4(1.0f), {-0.5f, 0.0f, 0.5f}) *
                                            glm::scale(glm::mat4(1.0f), {0.4f, 0.6f, 1.0f})},
                DrawItem{quad, rightMat, glm::translate(glm::mat4(1.0f), {0.5f, 0.0f, 0.5f}) *
                                             glm::scale(glm::mat4(1.0f), {0.4f, 0.6f, 1.0f})}};
            const Lighting brightLight{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 4.0f, 0.0f};
            const std::array<float, 4> clearColor{0.12f, 0.16f, 0.22f, 1.0f};

            // 1. 未准备显示 Shader 时 drawSceneTarget 拒绝执行。
            {
                ASSERT_TRUE(renderer.beginUiFrame());
                const auto output = renderer.prepareSceneTarget({128, 64});
                renderer.imgui().endFrame();
                EXPECT_THROW(renderer.drawSceneTarget(output, {{128, 64}, {glm::mat4(1), glm::mat4(1)}, draws}, clearColor),
                             Error::Exception);
                Detail::RendererReviewAccess::waitForFrames(renderer);
            }

            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);
            const Resource::ShaderResource emptyShader({}, {});
            EXPECT_THROW(renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment),
                         Error::Exception);
            EXPECT_THROW(renderer.prepareDisplayResources(emptyShader, *assets.displayFragment), Error::Exception);

            // 2. 非法曝光范围校验。
            {
                ASSERT_TRUE(renderer.beginUiFrame());
                const auto output = renderer.prepareSceneTarget({128, 64});
                renderer.imgui().endFrame();
                for (float badEv : {-8.5f, 8.5f, std::numeric_limits<float>::quiet_NaN()})
                {
                    FrameInput badInput{{128, 64}, {glm::mat4(1), glm::mat4(1)}, draws, brightLight};
                    badInput.display = {ToneMappingMode::Reinhard, badEv};
                    EXPECT_THROW(renderer.drawSceneTarget(output, badInput, clearColor), Error::Exception);
                }
                Detail::RendererReviewAccess::waitForFrames(renderer);
            }

            const auto renderAndCapture = [&](FrameExtent extent, std::span<const DrawItem> frameDraws,
                                              DisplaySettings display) {
                const auto uiExtent = renderer.beginUiFrame();
                EXPECT_TRUE(uiExtent);
                const auto output = renderer.prepareSceneTarget(extent);
                ImGui::Begin("Viewport");
                ImGui::Image(static_cast<ImTextureID>(renderer.sceneImage(output)),
                             ImVec2(float(extent.width), float(extent.height)));
                ImGui::End();
                renderer.imgui().endFrame();
                auto* drawData = renderer.imgui().drawData();
                FrameInput input{extent, {glm::mat4(1.0f), glm::mat4(1.0f)}, frameDraws, brightLight};
                input.display = display;
                renderer.drawSceneTarget(output, input, clearColor,
                                         [&](void* cmd) { renderer.imgui().recordDrawData(drawData, cmd); });
                std::vector<uint8_t> pixels;
                Detail::RendererReviewAccess::withSceneOutput(
                    renderer, output, [&](const auto& device, const auto& image) {
                        EXPECT_EQ(image.format(), VK_FORMAT_R8G8B8A8_UNORM);
                        pixels = TextureGpuReview::captureMip(device, image, 0, {extent.width, extent.height});
                    });
                return pixels;
            };

            // 3. 空场景直接输出原有 clearColor。
            {
                const auto px = renderAndCapture({128, 64}, {}, {ToneMappingMode::Reinhard, 0.0f});
                const size_t bgOff = (32u * 128u + 64u) * 4u;
                EXPECT_NEAR(px[bgOff + 0], clearColor[0] * 255.0f, 1.5f);
                EXPECT_NEAR(px[bgOff + 1], clearColor[1] * 255.0f, 1.5f);
                EXPECT_NEAR(px[bgOff + 2], clearColor[2] * 255.0f, 1.5f);
                EXPECT_EQ(px[bgOff + 3], 255u);
            }

            // 4. Off 模式：钳制到 [0, 1] 后编码一次 sRGB，EV 不影响输出；未覆盖背景仍为 clearColor。
            {
                const auto offNeg = renderAndCapture({128, 64}, draws, {ToneMappingMode::Off, -4.0f});
                const auto offPos = renderAndCapture({128, 64}, draws, {ToneMappingMode::Off, 4.0f});
                EXPECT_EQ(offNeg, offPos);
                // left = {2.0, 1.0, 0.4} -> clamped = {1.0, 1.0, 0.4}
                expectLinearPixel(offNeg, glm::vec3(1.0f, 1.0f, 0.4f), 32, 32, 128);
                // right = {4.0, 3.2, 2.0} -> clamped = {1.0, 1.0, 1.0}
                expectLinearPixel(offNeg, glm::vec3(1.0f, 1.0f, 1.0f), 96, 32, 128);
                const size_t bgOff = (32u * 128u + 64u) * 4u;
                EXPECT_NEAR(offNeg[bgOff + 0], clearColor[0] * 255.0f, 1.5f);
            }

            // 5. Reinhard 模式：参考像素正确、两个超亮样本可区分，且 +/-1 EV 曝光关系正确。
            {
                const auto reinhard0 = renderAndCapture({128, 64}, draws, {ToneMappingMode::Reinhard, 0.0f});
                const glm::vec3 leftHdr(2.0f, 1.0f, 0.4f);
                const glm::vec3 rightHdr(4.0f, 3.2f, 2.0f);
                expectLinearPixel(reinhard0, leftHdr / (1.0f + leftHdr), 32, 32, 128);
                expectLinearPixel(reinhard0, rightHdr / (1.0f + rightHdr), 96, 32, 128);
                const size_t leftOff = (32u * 128u + 32u) * 4u;
                const size_t rightOff = (32u * 128u + 96u) * 4u;
                EXPECT_GT(reinhard0[rightOff + 0], reinhard0[leftOff + 0] + 10);

                const auto reinhardPlus1 = renderAndCapture({128, 64}, draws, {ToneMappingMode::Reinhard, 1.0f});
                const glm::vec3 leftPlus1 = leftHdr * 2.0f;
                expectLinearPixel(reinhardPlus1, leftPlus1 / (1.0f + leftPlus1), 32, 32, 128);

                const auto reinhardMinus1 = renderAndCapture({128, 64}, draws, {ToneMappingMode::Reinhard, -1.0f});
                const glm::vec3 leftMinus1 = leftHdr * 0.5f;
                expectLinearPixel(reinhardMinus1, leftMinus1 / (1.0f + leftMinus1), 32, 32, 128);
            }

            // 6. 连续帧复用旧帧槽与视口尺寸重建（128x64 -> 192x96 -> 128x64）在 Required Validation 下无错误。
            for (const FrameExtent extent : {FrameExtent{192, 96}, FrameExtent{192, 96}, FrameExtent{128, 64},
                                             FrameExtent{128, 64}})
            {
                const auto px = renderAndCapture(extent, draws, {ToneMappingMode::Reinhard, 0.0f});
                const glm::vec3 rightHdr(4.0f, 3.2f, 2.0f);
                expectLinearPixel(px, rightHdr / (1.0f + rightHdr), (extent.width * 3u) / 4u, extent.height / 2u,
                                  extent.width);
            }
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, OffscreenFxaaSmoothingBaselineTogglesAndResizeValidation)
    {
        auto window = createWindow();
        SceneAssets assets;
        auto fxaaHold = Resource::ResourceManager::instance().hold<Resource::ShaderResource>(
            Resource::ResourceType::SHADER, "shaders/fxaa.frag.spv");
        ASSERT_TRUE(fxaaHold);
        const auto fxaaWords = fxaaHold.get<Resource::ShaderResource>()->words();
        fxaaHold.reset();
        const Resource::ShaderResource fxaaShader({}, fxaaWords);
        const Resource::ShaderResource emptyShader({}, {});
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);

            // 1. 准备顺序与未准备 FXAA 的错误边界。
            EXPECT_THROW(renderer.prepareFxaaResources(fxaaShader), Error::Exception);
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);
            EXPECT_THROW(renderer.prepareFxaaResources(emptyShader), Error::Exception);

            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const auto white = renderer.createTexture(whiteTex, TextureMipMode::BaseLevel, TextureColorSpace::Srgb);
            MaterialDesc matDesc{white, {0.9f, 0.9f, 0.9f}};
            matDesc.shading = ShadingMode::Unlit;
            const auto whiteMat = renderer.createMaterial(matDesc);

            // 固定对照几何：左侧高反差浅斜边三角（含大块纯白内部区域），右侧斜置细线条。
            const std::vector<MeshVertex> contrastVertices{
                // 三角形（索引 0, 1, 2）：斜边自 (-1.45, -0.30) 至 (-0.15, 0.45)，内部纯色覆盖右下角区域
                MeshVertex{{-1.45f, -0.30f, 0.0f}, {0.0f, 0.0f}},
                MeshVertex{{-0.15f, -0.75f, 0.0f}, {1.0f, 0.0f}},
                MeshVertex{{-0.15f, 0.45f, 0.0f}, {1.0f, 1.0f}},
                // 细线条（索引 3..6）：从 (0.20, -0.45) 至 (1.35, 0.45) 的约 1.2px 细斜带
                MeshVertex{{0.20f, -0.458f, 0.0f}, {0.0f, 0.0f}},
                MeshVertex{{0.20f, -0.442f, 0.0f}, {0.0f, 1.0f}},
                MeshVertex{{1.35f, 0.458f, 0.0f}, {1.0f, 1.0f}},
                MeshVertex{{1.35f, 0.442f, 0.0f}, {1.0f, 0.0f}},
            };
            const std::array<uint32_t, 9> contrastIndices{0, 1, 2, 3, 5, 4, 3, 6, 5};
            const auto mesh = renderer.createMesh(contrastVertices, contrastIndices);
            const std::array draws{DrawItem{mesh, whiteMat, glm::mat4(1.0f)}};
            releaseCpuAssets(assets);

            const std::array<float, 4> clearColor{0.04f, 0.05f, 0.08f, 1.0f};
            const auto makeCamera = [](FrameExtent extent) {
                const float aspect = float(extent.width) / float(extent.height);
                auto projection = glm::orthoRH_ZO(-aspect, aspect, -1.0f, 1.0f, 0.1f, 10.0f);
                projection[1][1] *= -1.0f;
                return CameraMatrices{
                    glm::lookAtRH(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                    projection};
            };

            // 未准备 FXAA 时以 fxaa=true 提交 drawSceneTarget 应明确抛错，但不破坏后续准备。
            {
                ASSERT_TRUE(renderer.beginUiFrame());
                const auto invalidFxaaOut = renderer.prepareSceneTarget({320, 180}, true);
                EXPECT_TRUE(invalidFxaaOut.fxaaEnabled());
                renderer.imgui().endFrame();
                FrameInput input{{320, 180}, makeCamera({320, 180}), draws};
                input.display = {ToneMappingMode::Reinhard, 0.0f};
                EXPECT_THROW(renderer.drawSceneTarget(invalidFxaaOut, input, clearColor), Error::Exception);
                Detail::RendererReviewAccess::waitForFrames(renderer);
            }

            renderer.prepareFxaaResources(fxaaShader);
            EXPECT_THROW(renderer.prepareFxaaResources(fxaaShader), Error::Exception);

            const auto renderSceneTarget = [&](FrameExtent extent, bool fxaa) {
                const auto uiExtent = renderer.beginUiFrame();
                EXPECT_TRUE(uiExtent);
                const auto output = renderer.prepareSceneTarget(extent, fxaa);
                EXPECT_EQ(output.fxaaEnabled(), fxaa);
                ImGui::Begin("Viewport");
                ImGui::Image(static_cast<ImTextureID>(renderer.sceneImage(output)),
                             ImVec2(float(extent.width), float(extent.height)));
                ImGui::End();
                renderer.imgui().endFrame();
                auto* drawData = renderer.imgui().drawData();
                FrameInput input{extent, makeCamera(extent), draws};
                input.display = {ToneMappingMode::Reinhard, 0.0f};
                renderer.drawSceneTarget(output, input, clearColor,
                                         [&](void* cmd) { renderer.imgui().recordDrawData(drawData, cmd); });
                std::vector<uint8_t> pixels;
                Detail::RendererReviewAccess::withSceneOutput(
                    renderer, output, [&](const auto& device, const auto& image) {
                        EXPECT_EQ(image.format(), VK_FORMAT_R8G8B8A8_UNORM);
                        pixels = TextureGpuReview::captureMip(device, image, 0, {extent.width, extent.height});
                    });
                return pixels;
            };

            const auto lumaOf = [&](const std::vector<uint8_t>& px, uint32_t x, uint32_t y, uint32_t w) {
                const size_t off = (size_t(y) * w + x) * 4u;
                return (int(px[off]) * 299 + int(px[off + 1]) * 587 + int(px[off + 2]) * 114) / 1000;
            };
            const auto pixelEqual = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t x,
                                       uint32_t y, uint32_t w) {
                const size_t off = (size_t(y) * w + x) * 4u;
                return a[off] == b[off] && a[off + 1] == b[off + 1] && a[off + 2] == b[off + 2] &&
                       a[off + 3] == b[off + 3];
            };

            // 2. 固定输入 320x180 下的开关基线对比，并保存对照图到 build/review/。
            constexpr FrameExtent BASE_EXTENT{320, 180};
            const auto offBaseline = renderSceneTarget(BASE_EXTENT, false);
            const auto onBaseline = renderSceneTarget(BASE_EXTENT, true);
            ASSERT_EQ(offBaseline.size(), size_t(BASE_EXTENT.width) * BASE_EXTENT.height * 4u);
            ASSERT_EQ(onBaseline.size(), offBaseline.size());

            // 远离边缘的背景纯色与三角形纯白内部像素保持一致。
            const int bgLuma = lumaOf(offBaseline, 16, 16, BASE_EXTENT.width);
            const int fgLuma = lumaOf(offBaseline, 130, 135, BASE_EXTENT.width);
            EXPECT_LT(bgLuma, 25);
            EXPECT_GT(fgLuma, 170);
            for (uint32_t y = 12; y <= 24; ++y)
                for (uint32_t x = 12; x <= 24; ++x)
                    EXPECT_TRUE(pixelEqual(offBaseline, onBaseline, x, y, BASE_EXTENT.width));
            for (uint32_t y = 128; y <= 142; ++y)
                for (uint32_t x = 124; x <= 136; ++x)
                    EXPECT_TRUE(pixelEqual(offBaseline, onBaseline, x, y, BASE_EXTENT.width));

            // 统计斜边区域内的中间过渡像素数量（关闭时应为 0，开启时应显著出现）。
            auto countEdgeIntermediates = [&](const std::vector<uint8_t>& px, uint32_t w, uint32_t h,
                                              uint32_t xMin, uint32_t xMax, uint32_t yMin, uint32_t yMax) {
                int count = 0;
                for (uint32_t y = yMin; y < yMax; ++y)
                    for (uint32_t x = xMin; x < xMax; ++x)
                    {
                        const size_t off = (size_t(y) * w + x) * 4u;
                        EXPECT_EQ(px[off + 3], 255u);
                        const int lum = lumaOf(px, x, y, w);
                        if (lum > bgLuma + 8 && lum < fgLuma - 8)
                            ++count;
                    }
                return count;
            };
            const int offDiagIntermediates = countEdgeIntermediates(offBaseline, 320, 180, 35, 135, 45, 115);
            const int onDiagIntermediates = countEdgeIntermediates(onBaseline, 320, 180, 35, 135, 45, 115);
            EXPECT_EQ(offDiagIntermediates, 0);
            EXPECT_GT(onDiagIntermediates, 20);

            // 右侧细线区域同样产生中间抗锯齿过渡像素。
            const int offLineIntermediates = countEdgeIntermediates(offBaseline, 320, 180, 175, 290, 45, 140);
            const int onLineIntermediates = countEdgeIntermediates(onBaseline, 320, 180, 175, 290, 45, 140);
            EXPECT_EQ(offLineIntermediates, 0);
            EXPECT_GT(onLineIntermediates, 20);

            // 保存固定机位对照结果到 build/review/fxaa-off.ppm 与 build/review/fxaa-on.ppm。
            const auto reviewDir = Utils::Utf8::toFsPath(RUNTIME_TEST_ASSET_DIR).parent_path().parent_path() / "review";
            WindowCapture::save({int(BASE_EXTENT.width), int(BASE_EXTENT.height), offBaseline},
                                reviewDir / "fxaa-off.ppm");
            WindowCapture::save({int(BASE_EXTENT.width), int(BASE_EXTENT.height), onBaseline},
                                reviewDir / "fxaa-on.ppm");

            // 3. 同一帧槽连续帧开关切换：关闭严格保持 LDR 基线，开启保持一致 FXAA 输出。
            for (const bool fxaa : {false, true, true, false, false, true})
            {
                const auto px = renderSceneTarget(BASE_EXTENT, fxaa);
                EXPECT_EQ(px, fxaa ? onBaseline : offBaseline);
            }

            // 4. 尺寸重建（480x270 -> 240x136）下开关及双帧槽输出均正确。
            for (const FrameExtent extent : {FrameExtent{480, 270}, FrameExtent{240, 136}})
            {
                for (int slotPass = 0; slotPass < 2; ++slotPass)
                {
                    const auto offPx = renderSceneTarget(extent, false);
                    const auto onPx = renderSceneTarget(extent, true);
                    EXPECT_TRUE(pixelEqual(offPx, onPx, 8, 8, extent.width));
                    const uint32_t xMin = extent.width / 10u;
                    const uint32_t xMax = (extent.width * 45u) / 100u;
                    const uint32_t yMin = extent.height / 4u;
                    const uint32_t yMax = (extent.height * 65u) / 100u;
                    EXPECT_EQ(countEdgeIntermediates(offPx, extent.width, extent.height, xMin, xMax, yMin, yMax), 0);
                    EXPECT_GT(countEdgeIntermediates(onPx, extent.width, extent.height, xMin, xMax, yMin, yMax), 10);
                }
            }

            // 5. 同 Renderer 下直接呈现路径 drawFrame 继续可用且不录制 FXAA。
            const auto extent = renderer.beginUiFrame();
            ASSERT_TRUE(extent);
            renderer.imgui().endFrame();
            renderer.drawFrame(FrameInput{*extent, makeCamera(*extent), draws}, clearColor);
        }
        expectCleanValidation();
    }

    TEST(VulkanTimestampMathTest, ComputesDurationWithValidBitsWrapAroundAndValidation)
    {
        using Vulkan::computeTimestampDurationMs;
        EXPECT_FALSE(computeTimestampDurationMs(10, 20, 0, 1.0f).has_value());
        EXPECT_FALSE(computeTimestampDurationMs(10, 20, 32, 0.0f).has_value());
        EXPECT_FALSE(computeTimestampDurationMs(10, 20, 32, -1.0f).has_value());

        // 普通递增与毫秒换算（1,000,000 ticks * 1 ns = 1.0 ms）。
        const auto normalMs = computeTimestampDurationMs(500, 1'000'500, 36, 1.0f);
        ASSERT_TRUE(normalMs.has_value());
        EXPECT_NEAR(*normalMs, 1.0, 1e-9);

        // 仅保留低 32 位有效位，忽略高位脏位。
        const auto maskedMs = computeTimestampDurationMs(0xABCD0000'00000100ULL, 0x12340000'00000500ULL, 32, 10.0f);
        ASSERT_TRUE(maskedMs.has_value());
        EXPECT_NEAR(*maskedMs, 0x400 * 10.0 / 1'000'000.0, 1e-9);

        // 32 位计数器回绕：0xFFFFFF00 -> 0x00000100 经过 0x200 ticks。
        const auto wrapped32Ms = computeTimestampDurationMs(0xFFFFFF00ULL, 0x00000100ULL, 32, 1000.0f);
        ASSERT_TRUE(wrapped32Ms.has_value());
        EXPECT_NEAR(*wrapped32Ms, 0x200 * 1000.0 / 1'000'000.0, 1e-9);

        // 64 位计数器回绕。
        const auto wrapped64Ms = computeTimestampDurationMs(std::numeric_limits<uint64_t>::max() - 99ULL, 100ULL, 64, 5.0f);
        ASSERT_TRUE(wrapped64Ms.has_value());
        EXPECT_NEAR(*wrapped64Ms, 200.0 * 5.0 / 1'000'000.0, 1e-9);
    }

    TEST_F(VulkanReviewTest, OffscreenViewportStatsLifecycleAndComparisonReport)
    {
        auto window = createWindow();
        SceneAssets assets;
        auto fxaaHold = Resource::ResourceManager::instance().hold<Resource::ShaderResource>(
            Resource::ResourceType::SHADER, "shaders/fxaa.frag.spv");
        auto shadowHold = Resource::ResourceManager::instance().hold<Resource::ShaderResource>(
            Resource::ResourceType::SHADER, "shaders/shadow.vert.spv");
        ASSERT_TRUE(fxaaHold);
        ASSERT_TRUE(shadowHold);
        const Resource::ShaderResource fxaaShader({}, fxaaHold.get<Resource::ShaderResource>()->words());
        const Resource::ShaderResource shadowShader({}, shadowHold.get<Resource::ShaderResource>()->words());
        fxaaHold.reset();
        shadowHold.reset();

        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);
            renderer.prepareFxaaResources(fxaaShader);
            renderer.prepareShadowResources(shadowShader);

            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const auto white = renderer.createTexture(whiteTex, TextureMipMode::BaseLevel, TextureColorSpace::Srgb);
            MaterialDesc groundMatDesc{white, {0.72f, 0.74f, 0.78f}};
            groundMatDesc.shading = ShadingMode::Lambert;
            const auto groundMat = renderer.createMaterial(groundMatDesc);
            MaterialDesc cubeMatDesc{white, {0.88f, 0.45f, 0.25f}};
            cubeMatDesc.shading = ShadingMode::Lambert;
            const auto cubeMat = renderer.createMaterial(cubeMatDesc);

            const auto cubeData = Demo::makeCubeMesh();
            const auto cubeMesh = renderer.createMesh(cubeData.vertices, cubeData.indices);
            const glm::mat4 groundModel =
                glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.6f, 0.0f)), glm::vec3(5.0f, 0.2f, 5.0f));
            const glm::mat4 casterA =
                glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(-0.8f, 0.2f, 0.2f)), glm::radians(28.0f),
                            glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::mat4 casterB =
                glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.9f, 0.0f, -0.5f)), glm::vec3(0.8f, 1.2f, 0.8f));
            const std::array draws{
                DrawItem{cubeMesh, groundMat, groundModel},
                DrawItem{cubeMesh, cubeMat, casterA},
                DrawItem{cubeMesh, cubeMat, casterB},
            };
            releaseCpuAssets(assets);

            // 1. 首帧提交前无完成样本；零尺寸不会产生样本。
            EXPECT_FALSE(renderer.lastViewportStats().hasCompletedSample);
            EXPECT_EQ(Detail::RendererReviewAccess::completedViewportSampleCount(renderer), 0u);
            EXPECT_FALSE(renderer.prepareSceneTarget({0, 0}, false).valid());
            EXPECT_EQ(Detail::RendererReviewAccess::completedViewportSampleCount(renderer), 0u);

            const std::array<float, 4> clearColor{0.08f, 0.10f, 0.14f, 1.0f};
            const auto makeCamera = [](FrameExtent extent) {
                const float aspect = float(extent.width) / float(extent.height);
                auto projection = glm::perspectiveRH_ZO(glm::radians(45.0f), aspect, 0.1f, 50.0f);
                projection[1][1] *= -1.0f;
                return CameraMatrices{
                    glm::lookAtRH(glm::vec3(2.8f, 2.2f, 3.6f), glm::vec3(0.0f, -0.1f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                    projection};
            };

            const auto submitOffscreen = [&](FrameExtent extent, bool shadows, bool fxaa,
                                             std::vector<uint8_t>* outPixels = nullptr) {
                const auto uiExtent = renderer.beginUiFrame();
                EXPECT_TRUE(uiExtent);
                const auto output = renderer.prepareSceneTarget(extent, fxaa);
                ImGui::Begin("Viewport");
                ImGui::Image(static_cast<ImTextureID>(renderer.sceneImage(output)),
                             ImVec2(float(extent.width), float(extent.height)));
                ImGui::End();
                renderer.imgui().endFrame();
                auto* drawData = renderer.imgui().drawData();

                FrameInput input{extent, makeCamera(extent), draws};
                input.display = {ToneMappingMode::Reinhard, 0.0f};
                input.lighting.direction = glm::normalize(glm::vec3(-0.6f, -1.0f, -0.4f));
                input.lighting.color = {1.0f, 0.96f, 0.90f};
                input.lighting.intensity = 2.6f;
                input.lighting.ambient = 0.25f;
                if (shadows)
                {
                    ShadowSettings settings;
                    settings.enabled = true;
                    settings.depthBias = 0.003f;
                    input.shadow = makeShadowFrame(settings, input.lighting.direction);
                    input.shadowDraws = draws;
                }

                renderer.drawSceneTarget(output, input, clearColor,
                                         [&](void* cmd) { renderer.imgui().recordDrawData(drawData, cmd); });
                if (outPixels)
                {
                    Detail::RendererReviewAccess::withSceneOutput(
                        renderer, output, [&](const auto& device, const auto& image) {
                            *outPixels = TextureGpuReview::captureMip(device, image, 0, {extent.width, extent.height});
                        });
                }
            };

            // 2. 生命周期：连续帧、开关阴影、drawFrame 不新增样本、尺寸重建。
            submitOffscreen({320, 180}, false, false);
            Detail::RendererReviewAccess::waitForFrames(renderer);
            auto stats = renderer.lastViewportStats();
            EXPECT_TRUE(stats.hasCompletedSample);
            EXPECT_EQ(stats.sceneDrawCount, 3u);
            EXPECT_EQ(stats.shadowDrawCount, 0u);
            if (stats.gpuTimestampAvailable)
            {
                ASSERT_TRUE(stats.gpuTimeMs.has_value());
                EXPECT_GE(*stats.gpuTimeMs, 0.0);
            }
            EXPECT_EQ(Detail::RendererReviewAccess::completedViewportSampleCount(renderer), 1u);

            // 开启阴影并等待该样本完成。
            submitOffscreen({320, 180}, true, false);
            Detail::RendererReviewAccess::waitForFrames(renderer);
            stats = renderer.lastViewportStats();
            EXPECT_TRUE(stats.hasCompletedSample);
            EXPECT_EQ(stats.sceneDrawCount, 3u);
            EXPECT_EQ(stats.shadowDrawCount, 3u);
            EXPECT_EQ(Detail::RendererReviewAccess::completedViewportSampleCount(renderer), 2u);

            // 直接呈现路径 drawFrame 不应新增离屏视口样本。
            {
                const auto uiExtent = renderer.beginUiFrame();
                ASSERT_TRUE(uiExtent);
                renderer.imgui().endFrame();
                renderer.drawFrame(FrameInput{*uiExtent, makeCamera(*uiExtent), draws}, clearColor);
                Detail::RendererReviewAccess::waitForFrames(renderer);
                EXPECT_EQ(Detail::RendererReviewAccess::completedViewportSampleCount(renderer), 2u);
            }

            // 尺寸重建后提交仍正常采集样本。
            submitOffscreen({480, 270}, false, true);
            Detail::RendererReviewAccess::waitForFrames(renderer);
            stats = renderer.lastViewportStats();
            EXPECT_EQ(stats.sceneDrawCount, 3u);
            EXPECT_EQ(stats.shadowDrawCount, 0u);
            EXPECT_EQ(Detail::RendererReviewAccess::completedViewportSampleCount(renderer), 3u);

            // 3. 固定条件三组对照（640x360）：全关 / 仅开 Shadows / 仅开 FXAA。
            constexpr FrameExtent COMPARE_EXTENT{640, 360};
            constexpr int WARMUP_FRAMES = 4;
            constexpr int SAMPLE_FRAMES = 11;
            struct GroupSpec
            {
                const char* label;
                const char* ppmName;
                bool shadows;
                bool fxaa;
            };
            const std::array<GroupSpec, 3> groups{{
                {"AllOff", "stats-off.ppm", false, false},
                {"ShadowsOnly", "stats-shadows.ppm", true, false},
                {"FxaaOnly", "stats-fxaa.ppm", false, true},
            }};

            const auto reviewDir = Utils::Utf8::toFsPath(RUNTIME_TEST_ASSET_DIR).parent_path().parent_path() / "review";
            std::filesystem::create_directories(reviewDir);
            std::ostringstream report;
            report << "Viewport Stats Comparison (640x360, Debug, Validation=Required)\n";
            report << "Warmup=" << WARMUP_FRAMES << ", Samples=" << SAMPLE_FRAMES << "\n";

            for (const auto& group : groups)
            {
                for (int i = 0; i < WARMUP_FRAMES; ++i)
                    submitOffscreen(COMPARE_EXTENT, group.shadows, group.fxaa);
                Detail::RendererReviewAccess::waitForFrames(renderer);

                std::vector<double> gpuTimes;
                uint32_t recordedSceneDraws = 0;
                uint32_t recordedShadowDraws = 0;
                std::vector<uint8_t> finalPixels;
                for (int i = 0; i < SAMPLE_FRAMES; ++i)
                {
                    submitOffscreen(COMPARE_EXTENT, group.shadows, group.fxaa,
                                    (i + 1 == SAMPLE_FRAMES) ? &finalPixels : nullptr);
                    Detail::RendererReviewAccess::waitForFrames(renderer);
                    const auto current = renderer.lastViewportStats();
                    EXPECT_TRUE(current.hasCompletedSample);
                    EXPECT_EQ(current.sceneDrawCount, 3u);
                    EXPECT_EQ(current.shadowDrawCount, group.shadows ? 3u : 0u);
                    recordedSceneDraws = current.sceneDrawCount;
                    recordedShadowDraws = current.shadowDrawCount;
                    if (current.gpuTimeMs.has_value())
                        gpuTimes.push_back(*current.gpuTimeMs);
                }

                WindowCapture::save({int(COMPARE_EXTENT.width), int(COMPARE_EXTENT.height), finalPixels},
                                    reviewDir / group.ppmName);

                double medianMs = 0.0;
                if (!gpuTimes.empty())
                {
                    std::sort(gpuTimes.begin(), gpuTimes.end());
                    medianMs = gpuTimes[gpuTimes.size() / 2];
                }
                report << group.label << ": medianGpuMs=" << medianMs << ", sceneDraws=" << recordedSceneDraws
                       << ", shadowDraws=" << recordedShadowDraws << ", ppm=" << group.ppmName << "\n";
            }

            std::ofstream reportFile(reviewDir / "viewport-stats-comparison.txt");
            reportFile << report.str();
            std::cout << report.str() << std::endl;
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, IblFloatCubemapUploadAndPbrMaterialResponsesUnderValidation)
    {
        const auto buildDir = Utils::Utf8::toFsPath(RUNTIME_TEST_ASSET_DIR).parent_path().parent_path();
        const auto buildAssetsDir = buildDir / "assets";
        const auto reviewDir = buildDir / "review";
        std::filesystem::create_directories(reviewDir);

        auto window = createWindow();
        SceneAssets assets;
        auto shadowHold = Resource::ResourceManager::instance().hold<Resource::ShaderResource>(
            Resource::ResourceType::SHADER, "shaders/shadow.vert.spv");
        ASSERT_TRUE(shadowHold);
        const Resource::ShaderResource shadowShader({}, shadowHold.get<Resource::ShaderResource>()->words());
        shadowHold.reset();

        const Resource::ShaderResource vertexShader({}, assets.vertex->words());
        const Resource::ShaderResource fragmentShader({}, assets.fragment->words());
        const Resource::ShaderResource displayVertexShader({}, assets.displayVertex->words());
        const Resource::ShaderResource displayFragmentShader({}, assets.displayFragment->words());
        const Resource::TextureResource refTex({}, assets.texture->width(), assets.texture->height(),
                                               assets.texture->pixels());
        releaseCpuAssets(assets);

        // 从构建部署根目录读取预生成的棚拍 IBL 资源与 Task 01 展示场景。
        auto io = FileIO::createPlatformFileIO();
        Resource::AssetSource buildSource(*io, buildAssetsDir);
        Resource::ResourceManager::instance().bindAssetSource(&buildSource);
        Resource::ResourceManager::instance().registerLoader(
            Resource::ResourceType::TEXTURE, std::make_unique<Resource::TextureLoader>(buildSource));
        Resource::ResourceManager::instance().registerLoader(
            Resource::ResourceType::MODEL, std::make_unique<Resource::ModelLoader>(buildSource));

        {
            Renderer renderer(*window, RendererValidationMode::Required);
            Texture irrTex, preTex, lutTex;
            {
                const auto irrHold = Resource::ResourceManager::instance().hold<Resource::FloatTextureResource>(
                    Resource::ResourceType::TEXTURE, "ibl/studio_irradiance.ktx");
                const auto preHold = Resource::ResourceManager::instance().hold<Resource::FloatTextureResource>(
                    Resource::ResourceType::TEXTURE, "ibl/studio_prefiltered.ktx");
                const auto lutHold = Resource::ResourceManager::instance().hold<Resource::FloatTextureResource>(
                    Resource::ResourceType::TEXTURE, "ibl/studio_brdf_lut.ktx");
                ASSERT_TRUE(irrHold);
                ASSERT_TRUE(preHold);
                ASSERT_TRUE(lutHold);
                const auto* irrData = irrHold.get<Resource::FloatTextureResource>();
                const auto* preData = preHold.get<Resource::FloatTextureResource>();
                const auto* lutData = lutHold.get<Resource::FloatTextureResource>();
                ASSERT_NE(irrData, nullptr);
                ASSERT_NE(preData, nullptr);
                ASSERT_NE(lutData, nullptr);

                // 1. 准备顺序与非法 IBL 调用校验。
                {
                    auto scoped = renderer.createResources();
                    const auto irrScoped = renderer.createTexture(scoped, *irrData);
                    const auto preScoped = renderer.createTexture(scoped, *preData);
                    const auto lutScoped = renderer.createTexture(scoped, *lutData);
                    EXPECT_TRUE(irrScoped.valid() && preScoped.valid() && lutScoped.valid());
                    EXPECT_THROW(renderer.prepareIblResources(irrScoped, preScoped, lutScoped), Error::Exception);
                    scoped.reset();
                    EXPECT_FALSE(irrScoped.valid() || preScoped.valid() || lutScoped.valid());
                }

                renderer.prepareSceneResources(vertexShader, fragmentShader);
                renderer.prepareDisplayResources(displayVertexShader, displayFragmentShader);
                renderer.prepareShadowResources(shadowShader);

                irrTex = renderer.createTexture(*irrData);
                preTex = renderer.createTexture(*preData);
                lutTex = renderer.createTexture(*lutData);
            }

            const auto white = renderer.createTexture(
                Resource::TextureResource({}, 1, 1, {255, 255, 255, 255}), TextureMipMode::BaseLevel,
                TextureColorSpace::Srgb);

            // 材质不得绑定浮点或立方体贴图；IBL 未准备前开启 iblEnabled 须抛出 RENDER；类型错配须拒绝。
            EXPECT_THROW(renderer.createMaterial({irrTex}), Error::Exception);
            EXPECT_THROW(renderer.createMaterial({lutTex}), Error::Exception);
            EXPECT_THROW(renderer.prepareIblResources(white, preTex, lutTex), Error::Exception);
            EXPECT_THROW(renderer.prepareIblResources(lutTex, preTex, lutTex), Error::Exception);
            EXPECT_THROW(renderer.prepareIblResources(irrTex, preTex, irrTex), Error::Exception);

            // 2. 构造球体网格用于光滑金属、粗糙金属与非金属响应检验。
            std::vector<MeshVertex> sphereVertices;
            std::vector<uint32_t> sphereIndices;
            constexpr uint32_t RINGS = 24;
            constexpr uint32_t SECTORS = 48;
            constexpr float PI = 3.14159265359f;
            for (uint32_t r = 0; r <= RINGS; ++r)
            {
                const float v = float(r) / RINGS;
                const float phi = v * PI;
                for (uint32_t s = 0; s <= SECTORS; ++s)
                {
                    const float u = float(s) / SECTORS;
                    const float theta = u * 2.0f * PI;
                    const glm::vec3 n{std::sin(phi) * std::sin(theta), std::cos(phi), std::sin(phi) * std::cos(theta)};
                    sphereVertices.push_back({{n.x * 0.5f, n.y * 0.5f, n.z * 0.5f},
                                              {u, v},
                                              {n.x, n.y, n.z},
                                              {std::cos(theta), 0.0f, -std::sin(theta), 1.0f}});
                }
            }
            for (uint32_t r = 0; r < RINGS; ++r)
            {
                for (uint32_t s = 0; s < SECTORS; ++s)
                {
                    const uint32_t a = r * (SECTORS + 1) + s;
                    const uint32_t b = a + SECTORS + 1;
                    sphereIndices.push_back(a);
                    sphereIndices.push_back(b);
                    sphereIndices.push_back(a + 1);
                    sphereIndices.push_back(a + 1);
                    sphereIndices.push_back(b);
                    sphereIndices.push_back(b + 1);
                }
            }
            const auto sphereMesh = renderer.createMesh(sphereVertices, sphereIndices);
            const auto quadData = Demo::makePlaneMesh();
            const auto quadMesh = renderer.createMesh(quadData.vertices, quadData.indices);

            MaterialDesc smoothMetalDesc{white, {0.95f, 0.95f, 0.95f}};
            smoothMetalDesc.shading = ShadingMode::Pbr;
            smoothMetalDesc.metallic = 1.0f;
            smoothMetalDesc.roughness = 0.05f;
            const auto smoothMetalMat = renderer.createMaterial(smoothMetalDesc);

            MaterialDesc roughMetalDesc{white, {0.95f, 0.95f, 0.95f}};
            roughMetalDesc.shading = ShadingMode::Pbr;
            roughMetalDesc.metallic = 1.0f;
            roughMetalDesc.roughness = 0.75f;
            const auto roughMetalMat = renderer.createMaterial(roughMetalDesc);

            MaterialDesc dielectricDesc{white, {0.82f, 0.28f, 0.18f}};
            dielectricDesc.shading = ShadingMode::Pbr;
            dielectricDesc.metallic = 0.0f;
            dielectricDesc.roughness = 0.85f;
            const auto dielectricMat = renderer.createMaterial(dielectricDesc);

            // IBL 关闭像素基线：安装 IBL 前后 iblEnabled=false 须输出完全一致的像素。
            const std::array<float, 4> blackClear{0.0f, 0.0f, 0.0f, 1.0f};
            auto proj = glm::perspectiveRH_ZO(glm::radians(38.0f), 900.0f / 320.0f, 0.1f, 20.0f);
            proj[1][1] *= -1.0f;
            const CameraMatrices sampleCamera{
                glm::lookAtRH(glm::vec3(0.0f, 0.0f, 4.1f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)), proj};
            const std::array sampleDraws{
                DrawItem{sphereMesh, smoothMetalMat, glm::translate(glm::mat4(1.0f), glm::vec3(-1.35f, 0.0f, 0.0f))},
                DrawItem{sphereMesh, roughMetalMat, glm::mat4(1.0f)},
                DrawItem{sphereMesh, dielectricMat, glm::translate(glm::mat4(1.0f), glm::vec3(1.35f, 0.0f, 0.0f))},
            };

            const auto captureSampleHdr = [&](const Lighting& lighting, bool iblEnabled) {
                std::vector<std::array<float, 4>> pixels;
                FrameInput input{{900, 320}, sampleCamera, sampleDraws, lighting};
                input.iblEnabled = iblEnabled;
                Detail::RendererReviewAccess::withHdrSceneTarget(
                    renderer, input, blackClear, [&](const auto& d, const auto& img) {
                        pixels = TextureGpuReview::captureRgba16f(d, img, {900, 320});
                    });
                return pixels;
            };
            const auto captureOffscreenLdr = [&](const FrameInput& input, const std::array<float, 4>& clear) {
                std::vector<uint8_t> pixels;
                const auto uiExtent = renderer.beginUiFrame();
                EXPECT_TRUE(uiExtent);
                const auto output = renderer.prepareSceneTarget(input.extent, false);
                ImGui::Begin("IBL");
                ImGui::Image(static_cast<ImTextureID>(renderer.sceneImage(output)),
                             ImVec2(float(input.extent.width), float(input.extent.height)));
                ImGui::End();
                renderer.imgui().endFrame();
                auto* drawData = renderer.imgui().drawData();
                renderer.drawSceneTarget(output, input, clear,
                                         [&](void* cmd) { renderer.imgui().recordDrawData(drawData, cmd); });
                Detail::RendererReviewAccess::withSceneOutput(
                    renderer, output, [&](const auto& d, const auto& img) {
                        pixels = TextureGpuReview::captureMip(d, img, 0, {input.extent.width, input.extent.height});
                    });
                return pixels;
            };

            // 尚未准备 IBL 时开启 iblEnabled 必须抛 RENDER。
            {
                FrameInput badInput{{320, 180}, sampleCamera, sampleDraws};
                badInput.iblEnabled = true;
                EXPECT_THROW(renderer.drawFrame(badInput, blackClear), Error::Exception);
            }

            const Lighting directLighting{{0.0f, -0.5f, -1.0f}, {1.0f, 0.95f, 0.9f}, 1.5f, 0.12f};
            const auto beforeIblPrepareOff = captureSampleHdr(directLighting, false);

            // 安装共享 IBL 资源，验证不可重复安装且原关闭基线严格不变。
            renderer.prepareIblResources(irrTex, preTex, lutTex);
            EXPECT_THROW(renderer.prepareIblResources(irrTex, preTex, lutTex), Error::Exception);
            const auto afterIblPrepareOff = captureSampleHdr(directLighting, false);
            EXPECT_EQ(beforeIblPrepareOff, afterIblPrepareOff);

            // 3. 方向光强度为零且常量 ambient=0：检验光滑金属、粗糙金属与非金属响应差异。
            const Lighting zeroLighting{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 0.0f, 0.0f};
            FrameInput zeroOffInput{{900, 320}, sampleCamera, sampleDraws, zeroLighting};
            zeroOffInput.display = {ToneMappingMode::Reinhard, 0.0f};
            zeroOffInput.iblEnabled = false;
            const auto zeroOffPx = captureOffscreenLdr(zeroOffInput, {0.06f, 0.07f, 0.09f, 1.0f});

            FrameInput zeroOnInput = zeroOffInput;
            zeroOnInput.iblEnabled = true;
            const auto zeroOnPx = captureOffscreenLdr(zeroOnInput, {0.06f, 0.07f, 0.09f, 1.0f});
            EXPECT_NE(zeroOnPx, zeroOffPx);
            WindowCapture::save({900, 320, zeroOnPx}, reviewDir / "ibl-samples-zero-light.ppm");

            struct RegionMetrics
            {
                double meanLuma = 0.0;
                double meanR = 0.0;
                double meanG = 0.0;
                double meanB = 0.0;
                double meanGrad = 0.0;
            };
            const auto measureSphereRegion = [&](std::span<const uint8_t> rgba, uint32_t xStart, uint32_t xEnd) {
                RegionMetrics m{};
                size_t count = 0;
                size_t gradCount = 0;
                const auto pxAt = [&](uint32_t x, uint32_t y, int c) -> double {
                    return rgba[(size_t(y) * 900u + x) * 4u + c] / 255.0;
                };
                for (uint32_t y = 120; y < 200; ++y)
                {
                    for (uint32_t x = xStart; x < xEnd; ++x)
                    {
                        const double r = pxAt(x, y, 0);
                        const double g = pxAt(x, y, 1);
                        const double b = pxAt(x, y, 2);
                        m.meanR += r;
                        m.meanG += g;
                        m.meanB += b;
                        m.meanLuma += 0.2126 * r + 0.7152 * g + 0.0722 * b;
                        ++count;
                        if (x + 1 < xEnd && y + 1 < 200)
                        {
                            const double l0 = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                            const double lx = 0.2126 * pxAt(x + 1, y, 0) + 0.7152 * pxAt(x + 1, y, 1) +
                                              0.0722 * pxAt(x + 1, y, 2);
                            const double ly = 0.2126 * pxAt(x, y + 1, 0) + 0.7152 * pxAt(x, y + 1, 1) +
                                              0.0722 * pxAt(x, y + 1, 2);
                            m.meanGrad += std::abs(lx - l0) + std::abs(ly - l0);
                            ++gradCount;
                        }
                    }
                }
                m.meanR /= count;
                m.meanG /= count;
                m.meanB /= count;
                m.meanLuma /= count;
                m.meanGrad /= gradCount;
                return m;
            };
            const auto smoothM = measureSphereRegion(zeroOnPx, 257, 337);
            const auto roughM = measureSphereRegion(zeroOnPx, 410, 490);
            const auto dielM = measureSphereRegion(zeroOnPx, 563, 643);
            EXPECT_GT(smoothM.meanLuma, 0.20);
            EXPECT_GT(roughM.meanLuma, 0.20);
            EXPECT_LT(roughM.meanGrad, smoothM.meanGrad * 0.65);
            EXPECT_GT(dielM.meanR, 0.50);
            EXPECT_GT(dielM.meanR, dielM.meanG * 1.25);
            EXPECT_GT(dielM.meanG, dielM.meanB * 1.05);

            // 4. 开启方向光与阴影时，IBL 之上仍保留直接光与 PCF 阴影衰减。
            const auto orthoProj = [] {
                auto p = glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 10.0f);
                p[1][1] *= -1.0f;
                return p;
            }();
            const CameraMatrices shadowCam{
                glm::lookAtRH(glm::vec3(0.0f, 1.5f, 3.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                orthoProj};
            const std::array<MeshVertex, 4> receiverVerts{{
                {{-2.0f, 0.0f, -2.0f}, {0, 0}, {0, 1, 0}, {1, 0, 0, 1}},
                {{2.0f, 0.0f, -2.0f}, {1, 0}, {0, 1, 0}, {1, 0, 0, 1}},
                {{2.0f, 0.0f, 2.0f}, {1, 1}, {0, 1, 0}, {1, 0, 0, 1}},
                {{-2.0f, 0.0f, 2.0f}, {0, 1}, {0, 1, 0}, {1, 0, 0, 1}},
            }};
            const std::array<uint32_t, 6> receiverIndices{0, 2, 1, 0, 3, 2};
            const auto xzQuadMesh = renderer.createMesh(receiverVerts, receiverIndices);
            MaterialDesc receiverDesc{white, {0.8f, 0.8f, 0.8f}};
            receiverDesc.shading = ShadingMode::Pbr;
            receiverDesc.metallic = 0.0f;
            receiverDesc.roughness = 0.8f;
            const auto matteReceiver = renderer.createMaterial(receiverDesc);
            const std::array shadowDraws{
                DrawItem{xzQuadMesh, matteReceiver, glm::mat4(1.0f)},
                DrawItem{xzQuadMesh, matteReceiver,
                         glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.8f, 0.0f)) *
                             glm::scale(glm::mat4(1.0f), glm::vec3(0.225f, 1.0f, 0.225f))},
            };
            ShadowSettings shadowSettings;
            shadowSettings.enabled = true;
            shadowSettings.depthBias = 0.002f;
            const Lighting topLight{{0.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 2.0f, 0.0f};
            FrameInput iblShadowInput{{384, 256}, shadowCam, shadowDraws, topLight};
            iblShadowInput.shadow = makeShadowFrame(shadowSettings, topLight.direction);
            iblShadowInput.shadowDraws = shadowDraws;
            iblShadowInput.display = {ToneMappingMode::Reinhard, 0.0f};
            iblShadowInput.iblEnabled = true;
            const auto shadowIblPx = captureOffscreenLdr(iblShadowInput, blackClear);
            const auto centerShadowed = shadowIblPx[(128u * 384u + 192u) * 4u];
            const auto sideLit = shadowIblPx[(128u * 384u + 80u) * 4u];
            EXPECT_GT(sideLit, centerShadowed + 20u);

            // 5. 在 Task 01 场景 pbr-showcase.json 上执行同机位 IBL OFF / ON 对照并记录 GPU 耗时。
            const auto showcaseDesc = Scene::readScene(*io, Utils::Utf8::toUtf8String(buildAssetsDir / "scenes/pbr-showcase.json"));
            Model::ModelManager modelManager(renderer);
            const auto rebuilt = Scene::rebuildScene(
                showcaseDesc, [&](const Model::ModelReference& ref) { return modelManager.resolve(ref); });
            constexpr FrameExtent SHOWCASE_EXTENT{800, 600};
            ShadowSettings showcaseShadow;
            showcaseShadow.enabled = true;
            showcaseShadow.depthBias = 0.003f;
            const auto extractedOff =
                Scene::extractSceneFrame(*rebuilt.scene, SHOWCASE_EXTENT, true, showcaseShadow, false);
            const auto extractedOn =
                Scene::extractSceneFrame(*rebuilt.scene, SHOWCASE_EXTENT, true, showcaseShadow, true);
            ASSERT_TRUE(extractedOff.has_value());
            ASSERT_TRUE(extractedOn.has_value());

            constexpr int WARMUP_FRAMES = 4;
            constexpr int SAMPLE_FRAMES = 11;
            std::ostringstream report;
            report << "Fixed Studio IBL Comparison (pbr-showcase.json, 800x600, Debug, Validation=Required)\n";
            report << "ExposureEv=" << showcaseDesc.display.exposureEv << ", ToneMapping=Reinhard, Shadows=ON(bias="
                   << showcaseShadow.depthBias << "), FXAA=OFF\n";
            report << "Warmup=" << WARMUP_FRAMES << ", Samples=" << SAMPLE_FRAMES << "\n";

            std::vector<uint8_t> showcaseOffPx, showcaseOnPx;
            for (const bool enableIbl : {false, true})
            {
                const auto& frameSrc = enableIbl ? *extractedOn : *extractedOff;
                for (int i = 0; i < WARMUP_FRAMES; ++i)
                    (void)captureOffscreenLdr(frameSrc.input(), showcaseDesc.clearColor);
                Detail::RendererReviewAccess::waitForFrames(renderer);

                std::vector<double> gpuTimes;
                uint32_t sceneDraws = 0;
                uint32_t shadowDrawsCount = 0;
                std::vector<uint8_t> lastPx;
                for (int i = 0; i < SAMPLE_FRAMES; ++i)
                {
                    lastPx = captureOffscreenLdr(frameSrc.input(), showcaseDesc.clearColor);
                    Detail::RendererReviewAccess::waitForFrames(renderer);
                    const auto stats = renderer.lastViewportStats();
                    EXPECT_TRUE(stats.hasCompletedSample);
                    sceneDraws = stats.sceneDrawCount;
                    shadowDrawsCount = stats.shadowDrawCount;
                    if (stats.gpuTimeMs.has_value())
                        gpuTimes.push_back(*stats.gpuTimeMs);
                }
                double medianMs = 0.0;
                if (!gpuTimes.empty())
                {
                    std::sort(gpuTimes.begin(), gpuTimes.end());
                    medianMs = gpuTimes[gpuTimes.size() / 2];
                }
                const char* ppmName = enableIbl ? "ibl-showcase-on.ppm" : "ibl-showcase-off.ppm";
                WindowCapture::save({int(SHOWCASE_EXTENT.width), int(SHOWCASE_EXTENT.height), lastPx},
                                    reviewDir / ppmName);
                if (enableIbl)
                    showcaseOnPx = std::move(lastPx);
                else
                    showcaseOffPx = std::move(lastPx);
                report << (enableIbl ? "IblOn" : "IblOff") << ": medianGpuMs=" << medianMs
                       << ", sceneDraws=" << sceneDraws << ", shadowDraws=" << shadowDrawsCount
                       << ", ppm=" << ppmName << "\n";
            }
            EXPECT_NE(showcaseOnPx, showcaseOffPx);
            std::ofstream reportOut(reviewDir / "ibl-comparison-report.txt");
            reportOut << report.str();
            std::cout << report.str() << std::endl;
        }
        Resource::ResourceManager::instance().reset();
        Resource::ResourceManager::instance().bindAssetSource(nullptr);
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, MaterialParameterModesFixedOverrideSharedModelIsolationAndZeroTextureReuploadUnderValidation)
    {
        const auto buildDir = Utils::Utf8::toFsPath(RUNTIME_TEST_ASSET_DIR).parent_path().parent_path();
        const auto buildAssetsDir = buildDir / "assets";
        const auto reviewDir = buildDir / "review";
        std::filesystem::create_directories(reviewDir);

        auto window = createWindow();
        SceneAssets assets;
        const Resource::ShaderResource vertexShader({}, assets.vertex->words());
        const Resource::ShaderResource fragmentShader({}, assets.fragment->words());
        const Resource::ShaderResource displayVertexShader({}, assets.displayVertex->words());
        const Resource::ShaderResource displayFragmentShader({}, assets.displayFragment->words());
        releaseCpuAssets(assets);

        auto io = FileIO::createPlatformFileIO();
        Resource::AssetSource buildSource(*io, buildAssetsDir);
        Resource::ResourceManager::instance().bindAssetSource(&buildSource);
        Resource::ResourceManager::instance().registerLoader(
            Resource::ResourceType::TEXTURE, std::make_unique<Resource::TextureLoader>(buildSource));
        Resource::ResourceManager::instance().registerLoader(
            Resource::ResourceType::MODEL, std::make_unique<Resource::ModelLoader>(buildSource));

        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(vertexShader, fragmentShader);
            renderer.prepareDisplayResources(displayVertexShader, displayFragmentShader);
            {
                const auto irrHold = Resource::ResourceManager::instance().hold<Resource::FloatTextureResource>(
                    Resource::ResourceType::TEXTURE, "ibl/studio_irradiance.ktx");
                const auto preHold = Resource::ResourceManager::instance().hold<Resource::FloatTextureResource>(
                    Resource::ResourceType::TEXTURE, "ibl/studio_prefiltered.ktx");
                const auto lutHold = Resource::ResourceManager::instance().hold<Resource::FloatTextureResource>(
                    Resource::ResourceType::TEXTURE, "ibl/studio_brdf_lut.ktx");
                ASSERT_TRUE(irrHold && preHold && lutHold);
                renderer.prepareIblResources(renderer.createTexture(*irrHold.get<Resource::FloatTextureResource>()),
                                             renderer.createTexture(*preHold.get<Resource::FloatTextureResource>()),
                                             renderer.createTexture(*lutHold.get<Resource::FloatTextureResource>()));
            }

            // 1. 验证 MaterialParameterMode 非法枚举拒绝，以及固定粗糙度/金属度替换贴图通道。
            const auto white = renderer.createTexture(
                Resource::TextureResource({}, 1, 1, {255, 255, 255, 255}), TextureMipMode::BaseLevel,
                TextureColorSpace::Srgb);
            // 贴图 G=230 (~0.902 粗糙), B=0 (0.0 金属)。
            const auto roughNonMetalTex = renderer.createTexture(
                Resource::TextureResource({}, 1, 1, {0, 230, 0, 255}), TextureMipMode::BaseLevel,
                TextureColorSpace::Linear);
            {
                MaterialDesc invalidDesc{white};
                invalidDesc.shading = ShadingMode::Pbr;
                invalidDesc.roughnessMode = static_cast<MaterialParameterMode>(7);
                EXPECT_THROW(renderer.createMaterial(invalidDesc), Error::Exception);
                invalidDesc.roughnessMode = MaterialParameterMode::TextureFactor;
                invalidDesc.metallicMode = static_cast<MaterialParameterMode>(7);
                EXPECT_THROW(renderer.createMaterial(invalidDesc), Error::Exception);
            }

            MaterialDesc texFactorDesc{white};
            texFactorDesc.shading = ShadingMode::Pbr;
            texFactorDesc.metallicRoughnessTexture = roughNonMetalTex;
            texFactorDesc.roughness = 1.0f;
            texFactorDesc.metallic = 1.0f;
            const auto matTexFactor = renderer.createMaterial(texFactorDesc);

            MaterialDesc fixedOverrideDesc = texFactorDesc;
            fixedOverrideDesc.roughnessMode = MaterialParameterMode::FixedValue;
            fixedOverrideDesc.roughness = 0.05f;
            fixedOverrideDesc.metallicMode = MaterialParameterMode::FixedValue;
            fixedOverrideDesc.metallic = 1.0f;
            const auto matFixedMetal = renderer.createMaterial(fixedOverrideDesc);

            const auto quadData = Demo::makePlaneMesh();
            const std::array<MeshVertex, 4> tangentQuad{{
                {{-1, -1, 0}, {0, 0}, {0, 0, 1}, {1, 0, 0, 1}},
                {{1, -1, 0}, {1, 0}, {0, 0, 1}, {1, 0, 0, 1}},
                {{1, 1, 0}, {1, 1}, {0, 0, 1}, {1, 0, 0, 1}},
                {{-1, 1, 0}, {0, 1}, {0, 0, 1}, {1, 0, 0, 1}},
            }};
            const std::array<uint32_t, 6> quadIndices{0, 1, 2, 0, 2, 3};
            const auto tangentMesh = renderer.createMesh(tangentQuad, quadIndices);

            auto ortho = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 10.0f);
            ortho[1][1] *= -1.0f;
            const CameraMatrices cam{
                glm::lookAtRH(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)), ortho};
            const auto captureHdrCenter = [&](const Material& mat, const Lighting& l, bool ibl) {
                std::vector<std::array<float, 4>> hdr;
                FrameInput input{{64, 64}, cam, std::array{DrawItem{tangentMesh, mat, glm::mat4(1.0f)}}, l};
                input.iblEnabled = ibl;
                Detail::RendererReviewAccess::withHdrSceneTarget(
                    renderer, input, {0, 0, 0, 1}, [&](const auto& d, const auto& img) {
                        hdr = TextureGpuReview::captureRgba16f(d, img, {64, 64});
                    });
                return hdr[(32u * 64u + 32u)];
            };
            // 正向光源与视线对准半向量 H=(0,0,1)：高粗糙度(0.9)高光极弱，固定粗糙度(0.05) + 固定金属度(1.0)产生强高光。
            const Lighting frontLight{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 1.0f, 0.0f};
            const auto factorPx = captureHdrCenter(matTexFactor, frontLight, false);
            const auto fixedPx = captureHdrCenter(matFixedMetal, frontLight, false);
            EXPECT_GT(fixedPx[0], factorPx[0] * 10.0f);

            // 2. 加载共享 Avocado 模型并建立双实体，验证 PreparedModel 槽位映射、隔离、零贴图重传及 Fence 回收。
            Model::PrepareOptions pbrOpts;
            pbrOpts.mipMode = TextureMipMode::FullChain;
            pbrOpts.sampler = {TextureFilter::Linear, TextureFilter::Linear, MipFilter::Linear};
            pbrOpts.shading = ShadingMode::Pbr;
            const auto avocado = Model::prepareModel(renderer, "models/avocado/gltf/avocado.gltf", pbrOpts);
            ASSERT_EQ(avocado->materialSlots().size(), 1u);
            EXPECT_EQ(avocado->materialSlots()[0].sourceMaterialIndex, 0u);
            ASSERT_EQ(avocado->drawMaterialSlots().size(), avocado->draws().size());
            EXPECT_EQ(avocado->drawMaterialSlots()[0], 0u);

            const auto countsBeforeOverride = Detail::RendererReviewAccess::resourceCounts(renderer);
            const std::string logsBefore = messages_.str();
            const size_t uploadsBefore = TextureGpuReview::countSceneTextureUploads(logsBefore);

            Scene::Scene scene;
            scene.setCamera(Scene::Camera({0.0f, 0.0f, 2.6f}));
            scene.setLighting({{0.5f, -0.5f, -0.7f}, {1.0f, 0.96f, 0.9f}, 1.2f, 0.12f});
            const auto leftId =
                scene.createEntity(avocado, Scene::Transform({-0.65f, -0.75f, 0.0f}, {1, 0, 0, 0}, glm::vec3(24.0f)),
                                   true, "Left");
            const auto rightId =
                scene.createEntity(avocado, Scene::Transform({0.65f, -0.75f, 0.0f}, {1, 0, 0, 0}, glm::vec3(24.0f)),
                                   true, "Right");

            const auto renderSceneLdr = [&](FrameExtent ext) {
                std::vector<uint8_t> pixels;
                const auto extracted = Scene::extractSceneFrame(scene, ext, true, {}, true);
                EXPECT_TRUE(extracted.has_value());
                const auto uiExt = renderer.beginUiFrame();
                EXPECT_TRUE(uiExt);
                const auto output = renderer.prepareSceneTarget(ext, false);
                ImGui::Begin("OverrideReview");
                ImGui::Image(static_cast<ImTextureID>(renderer.sceneImage(output)),
                             ImVec2(float(ext.width), float(ext.height)));
                ImGui::End();
                renderer.imgui().endFrame();
                auto* dd = renderer.imgui().drawData();
                renderer.drawSceneTarget(output, extracted->input(), {0.08f, 0.095f, 0.125f, 1.0f},
                                         [&](void* cmd) { renderer.imgui().recordDrawData(dd, cmd); });
                Detail::RendererReviewAccess::withSceneOutput(
                    renderer, output, [&](const auto& d, const auto& img) {
                        pixels = TextureGpuReview::captureMip(d, img, 0, {ext.width, ext.height});
                    });
                return pixels;
            };

            const auto basePx = renderSceneLdr({640, 480});

            // 仅对左侧实体应用覆盖（红色染色 + 固定光滑金属），右侧实体保持原样。
            Scene::MaterialOverride ov{
                0u,
                {1.0f, 0.25f, 0.2f},
                MaterialParameterMode::FixedValue,
                0.12f,
                MaterialParameterMode::FixedValue,
                1.0f,
            };
            auto compiled = Model::prepareMaterialOverride(renderer, *avocado, ov.materialSlot, ov.color,
                                                           ov.roughnessMode, ov.roughness, ov.metallicMode, ov.metallic);
            ASSERT_TRUE(scene.setMaterialOverride(leftId, ov, compiled));
            compiled.reset();

            // 验证零纹理/网格重传，只新增 1 份独立材质绑定。
            const auto countsAfterOverride = Detail::RendererReviewAccess::resourceCounts(renderer);
            EXPECT_EQ(countsAfterOverride.meshes, countsBeforeOverride.meshes);
            EXPECT_EQ(countsAfterOverride.textures, countsBeforeOverride.textures);
            EXPECT_EQ(countsAfterOverride.materials, countsBeforeOverride.materials + 1u);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), uploadsBefore);

            const auto overriddenPx = renderSceneLdr({640, 480});
            WindowCapture::save({640, 480, overriddenPx}, reviewDir / "material-override-isolation.ppm");

            // 验证左半屏变化且右半屏（共享同模型的右侧实体）逐像素严格不变。
            bool leftChanged = false;
            for (uint32_t y = 0; y < 480; ++y)
            {
                for (uint32_t x = 0; x < 310; ++x)
                {
                    const size_t off = (size_t(y) * 640u + x) * 4u;
                    if (overriddenPx[off] != basePx[off] || overriddenPx[off + 1] != basePx[off + 1] ||
                        overriddenPx[off + 2] != basePx[off + 2])
                    {
                        leftChanged = true;
                        break;
                    }
                }
                if (leftChanged)
                    break;
            }
            EXPECT_TRUE(leftChanged);
            for (uint32_t y = 0; y < 480; ++y)
            {
                for (uint32_t x = 330; x < 640; ++x)
                {
                    const size_t off = (size_t(y) * 640u + x) * 4u;
                    ASSERT_EQ(overriddenPx[off + 0], basePx[off + 0]);
                    ASSERT_EQ(overriddenPx[off + 1], basePx[off + 1]);
                    ASSERT_EQ(overriddenPx[off + 2], basePx[off + 2]);
                }
            }

            // 连续替换覆盖并重置，验证在途帧 Fence 完成前安全且最终回收独立覆盖材质资源。
            for (int k = 1; k <= 3; ++k)
            {
                ov.roughness = 0.1f * float(k);
                auto nextCompiled =
                    Model::prepareMaterialOverride(renderer, *avocado, ov.materialSlot, ov.color, ov.roughnessMode,
                                                   ov.roughness, ov.metallicMode, ov.metallic);
                ASSERT_TRUE(scene.setMaterialOverride(leftId, ov, std::move(nextCompiled)));
                (void)renderSceneLdr({640, 480});
            }
            ASSERT_TRUE(scene.clearMaterialOverride(leftId, 0u));
            const auto restoredPx = renderSceneLdr({640, 480});
            EXPECT_EQ(restoredPx, basePx);
            Detail::RendererReviewAccess::waitForFrames(renderer);
            EXPECT_EQ(Detail::RendererReviewAccess::resourceCounts(renderer).materials, countsBeforeOverride.materials);

            // 验证 Scene 1.2 捕获、序列化与 rebuildScene GPU 重建恢复。
            ASSERT_TRUE(scene.setMaterialOverride(
                leftId, ov,
                Model::prepareMaterialOverride(renderer, *avocado, ov.materialSlot, ov.color, ov.roughnessMode,
                                               ov.roughness, ov.metallicMode, ov.metallic)));
            const auto desc12 = Scene::captureScene(scene, {0.08f, 0.095f, 0.125f, 1.0f});
            const auto decoded12 = Scene::decodeScene(Scene::encodeScene(desc12));
            const auto rebuilt12 = Scene::rebuildScene(
                decoded12, [&](const Model::ModelReference&) { return avocado; },
                [&](const Model::PreparedModel& model, const Scene::MaterialOverride& item) {
                    return Model::prepareMaterialOverride(renderer, model, item.materialSlot, item.color,
                                                          item.roughnessMode, item.roughness, item.metallicMode,
                                                          item.metallic);
                });
            ASSERT_EQ(rebuilt12.scene->entities().size(), 2u);
            EXPECT_EQ(rebuilt12.scene->entities()[0].materialOverrides.size(), 1u);
            EXPECT_TRUE(rebuilt12.scene->entities()[1].materialOverrides.empty());
            ASSERT_TRUE(rebuilt12.scene->entities()[0].materialOverrides[0].compiled != nullptr);
            EXPECT_TRUE(rebuilt12.scene->entities()[0].materialOverrides[0].compiled->material.valid());
            EXPECT_TRUE(rebuilt12.scene->find(rebuilt12.scene->entities()[1].id)->materialOverrides.empty());
            EXPECT_EQ(scene.find(rightId)->materialOverrides.size(), 0u);
        }
        Resource::ResourceManager::instance().reset();
        Resource::ResourceManager::instance().bindAssetSource(nullptr);
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, AdaptiveShadowFitNormalBiasAndResolutionSwitchUnderValidation)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            auto& manager = Resource::ResourceManager::instance();
            const auto shadowVert =
                manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, "shaders/shadow.vert.spv");
            const auto shadowMaskVert = manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER,
                                                                               "shaders/shadow_mask.vert.spv");
            const auto shadowMaskFrag = manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER,
                                                                               "shaders/shadow_mask.frag.spv");
            ASSERT_TRUE(shadowVert && shadowMaskVert && shadowMaskFrag);
            renderer.prepareShadowResources(*shadowVert.get<Resource::ShaderResource>(),
                                            *shadowMaskVert.get<Resource::ShaderResource>(),
                                            *shadowMaskFrag.get<Resource::ShaderResource>());
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);

            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const auto white = renderer.createTexture(whiteTex);
            const auto plane = createLitPlane(renderer);
            MaterialDesc lambertDesc{white};
            lambertDesc.shading = ShadingMode::Lambert;
            lambertDesc.doubleSided = true;
            const auto mat = renderer.createMaterial(lambertDesc);

            // 1. 大范围偏移场景：在 (25, 0, 0) 处放置投射体与接收面（超出固定 {0,0,0}, halfExtent{6,6}）。
            constexpr FrameExtent extent{64, 64};
            constexpr size_t center = 32u * 64u + 32u;
            const auto offCenterProj = [&] {
                auto p = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 30.0f);
                p[1][1] *= -1.0f;
                return CameraMatrices{
                    glm::lookAtRH(glm::vec3(25.0f, 0.0f, 3.0f), glm::vec3(25.0f, 0.0f, 0.0f), glm::vec3(0, 1, 0)), p};
            }();
            const std::array offsetReceivers{
                DrawItem{plane, mat, glm::translate(glm::mat4(1.0f), {25.0f, 0.0f, 0.0f})},
            };
            const std::array offsetCasters{
                DrawItem{plane, mat,
                         glm::translate(glm::mat4(1.0f), {25.0f, 0.0f, 1.0f}) *
                             glm::scale(glm::mat4(1.0f), {0.4f, 0.4f, 1.0f})},
            };
            const Lighting light{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 1.0f, 0.0f};
            ShadowSettings fixedSettings;
            fixedSettings.enabled = true;
            FrameInput fixedInput{extent, offCenterProj, offsetReceivers, light,
                                  makeShadowFrame(fixedSettings, light.direction), offsetCasters};
            const auto hdrFixed = captureHdr(renderer, fixedInput);
            ASSERT_GT(hdrFixed.size(), center);
            EXPECT_GT(hdrFixed[center][0], 0.9f);  // 固定范围漏失阴影，保持全亮

            ShadowSettings fitSettings = fixedSettings;
            fitSettings.fitToBounds = true;
            fitSettings.resolution = 2048u;
            const Math::Aabb casterBounds{{24.0f, -1.0f, 0.0f}, {26.0f, 1.0f, 1.0f}};
            FrameInput fitInput{extent, offCenterProj, offsetReceivers, light,
                                makeShadowFrameForBounds(fitSettings, light.direction, casterBounds), offsetCasters};
            const auto hdrFitted = captureHdr(renderer, fitInput);
            ASSERT_GT(hdrFitted.size(), center);
            EXPECT_LT(hdrFitted[center][0], 0.1f);  // 自适应拟合精准覆盖并遮挡

            // 2. 斜面自阴影（Shadow Acne）与 normalBias 修复：75 度倾斜正面受 +Z 光源斜照。
            const auto slopeModel = glm::rotate(glm::mat4(1.0f), glm::radians(75.0f), {0.0f, 1.0f, 0.0f}) *
                                    glm::scale(glm::mat4(1.0f), {3.0f, 3.0f, 1.0f});
            const std::array slopeDraw{DrawItem{plane, mat, slopeModel}};
            const Lighting slopeLight{{0.0f, 0.0f, -1.0f}, {1, 1, 1}, 1.0f, 0.0f};
            ShadowSettings acneSettings;
            acneSettings.enabled = true;
            acneSettings.halfExtent = {6.0f, 6.0f};
            acneSettings.nearPlane = 1.0f;
            acneSettings.farPlane = 9.0f;
            acneSettings.depthBias = 0.0f;
            acneSettings.normalBias = 0.0f;
            acneSettings.resolution = 512u;
            FrameInput acneInput{extent,
                                 controlledFrame(extent, slopeDraw).camera,
                                 slopeDraw,
                                 slopeLight,
                                 makeShadowFrame(acneSettings, slopeLight.direction),
                                 slopeDraw};
            const auto hdrAcne = captureHdr(renderer, acneInput);
            float minAcne = 1.0f, maxAcne = 0.0f;
            for (int y = 26; y <= 38; ++y)
                for (int x = 26; x <= 38; ++x)
                {
                    const float r = hdrAcne[size_t(y) * 64u + size_t(x)][0];
                    minAcne = std::min(minAcne, r);
                    maxAcne = std::max(maxAcne, r);
                }
            EXPECT_GT(maxAcne - minAcne, 0.05f);  // 零偏移斜面存在明显自阴影条纹

            acneSettings.normalBias = 0.05f;
            acneInput.shadow = makeShadowFrame(acneSettings, slopeLight.direction);
            const auto hdrClean = captureHdr(renderer, acneInput);
            float minClean = 1.0f, maxClean = 0.0f;
            for (int y = 26; y <= 38; ++y)
                for (int x = 26; x <= 38; ++x)
                {
                    const float r = hdrClean[size_t(y) * 64u + size_t(x)][0];
                    minClean = std::min(minClean, r);
                    maxClean = std::max(maxClean, r);
                }
            const float expectedDiffuse = std::cos(glm::radians(75.0f));
            EXPECT_NEAR(minClean, expectedDiffuse, 0.005f);
            EXPECT_NEAR(maxClean, minClean, 0.005f);  // normalBias 消除自阴影，表面光滑一致受光

            // 3. 在正式 offscreen drawSceneTarget 路径下连续切换阴影分辨率 1024 <-> 2048，验证重建安全与零 Validation 错误。
            for (const uint32_t res : {1024u, 2048u, 1024u})
            {
                fitSettings.resolution = res;
                fitInput.shadow = makeShadowFrameForBounds(fitSettings, light.direction, casterBounds);
                for (int k = 0; k < 2; ++k)
                {
                    window->pollEvents();
                    ASSERT_TRUE(renderer.beginUiFrame());
                    const auto output = renderer.prepareSceneTarget(extent);
                    renderer.imgui().endFrame();
                    renderer.drawSceneTarget(output, fitInput, {0, 0, 0, 1});
                }
            }
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, CascadedShadowMapsCloseSharpnessFarCoverageAlphaMaskAndResizeUnderValidation)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            auto& manager = Resource::ResourceManager::instance();
            const auto shadowVert =
                manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, "shaders/shadow.vert.spv");
            const auto shadowMaskVert = manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER,
                                                                               "shaders/shadow_mask.vert.spv");
            const auto shadowMaskFrag = manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER,
                                                                               "shaders/shadow_mask.frag.spv");
            ASSERT_TRUE(shadowVert && shadowMaskVert && shadowMaskFrag);
            renderer.prepareShadowResources(*shadowVert.get<Resource::ShaderResource>(),
                                            *shadowMaskVert.get<Resource::ShaderResource>(),
                                            *shadowMaskFrag.get<Resource::ShaderResource>());
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);

            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const auto white = renderer.createTexture(whiteTex);
            const auto plane = createLitPlane(renderer);
            MaterialDesc lambertDesc{white};
            lambertDesc.shading = ShadingMode::Lambert;
            lambertDesc.doubleSided = true;
            const auto mat = renderer.createMaterial(lambertDesc);

            // Alpha Mask 贴图：左半透明(alpha=0)，右半不透明(alpha=255)。
            const Resource::TextureResource maskTex({}, 2, 1, {255, 255, 255, 0, 255, 255, 255, 255});
            MaterialDesc maskDesc{renderer.createTexture(maskTex)};
            maskDesc.shading = ShadingMode::Lambert;
            maskDesc.alphaMode = AlphaMode::Mask;
            maskDesc.alphaCutoff = 0.5f;
            maskDesc.doubleSided = true;
            const auto maskMat = renderer.createMaterial(maskDesc);

            constexpr FrameExtent extent{128, 128};
            auto proj = glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
            proj[1][1] *= -1.0f;
            const CameraMatrices cam{
                glm::lookAtRH(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                proj};
            const Lighting light{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 1.0f, 0.0f};

            // 窄细近景投射体（宽度 0.08 在 z = -2.5，投影到 z = -3.0 接收面中心）+ 远景物体（在 z = -35.0 投影到 z = -36.0）。
            const std::array nearDraws{
                DrawItem{plane, mat,
                         glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, -3.0f}) *
                             glm::scale(glm::mat4(1.0f), {1.5f, 1.5f, 1.0f})},
                DrawItem{plane, mat,
                         glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, -2.5f}) *
                             glm::scale(glm::mat4(1.0f), {0.04f, 0.5f, 1.0f})},
            };

            const Math::Aabb sceneBounds{{-15.0f, -15.0f, -45.0f}, {15.0f, 15.0f, 0.0f}};
            ShadowSettings singleLarge;
            singleLarge.enabled = true;
            singleLarge.resolution = 512u;
            singleLarge.cascadeCount = 1u;
            singleLarge.maxDistance = 60.0f;
            singleLarge.normalBias = 0.0f;
            // 单级大视锥覆盖 60 米范围时单个纹素大，窄细遮挡体投影会被严重模糊削弱。
            const auto singleShadow =
                makeCascadedShadowFrame(singleLarge, light.direction, cam, 0.1f, 100.0f, sceneBounds);
            ShadowSettings csmSettings = singleLarge;
            csmSettings.cascadeCount = 3u;
            csmSettings.cascadeSplitLambda = 0.75f;
            const auto csmShadow =
                makeCascadedShadowFrame(csmSettings, light.direction, cam, 0.1f, 100.0f, sceneBounds);
            ASSERT_TRUE(singleShadow && csmShadow);
            ASSERT_LT(csmShadow->cascadeSplits[0], 3.0f + 5.0f);  // 确保第 0 级紧密包住近处 z=-3

            // 检查近景阴影对比：在接收面（避开窄投射体正面直视的位置——或者把窄投射体放在光路上，但相机略微偏视角？
            // Wait：此处 light={0,0,-1} 与视线方向相同！如果投射体在 z=-2.5、相机在原点，相机也会直接看到投射体本身！
            // 把光线斜向照射：方向 (-0.5, 0, -1)，投射体置于 (0.5, 0, -2.0) 让阴影斜投射到 (-0.0, 0, -3.0) 接收面中心，
            // 从而相机正对 (-0.0, 0, -3.0) 只看到接收面上的清晰阴影而不被投射体挡住视线！
            const Lighting angledLight{glm::normalize(glm::vec3(-0.5f, 0.0f, -1.0f)), {1.0f, 1.0f, 1.0f}, 1.0f, 0.0f};
            const std::array angledNearDraws{
                DrawItem{plane, mat,
                         glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, -3.0f}) *
                             glm::scale(glm::mat4(1.0f), {2.0f, 2.0f, 1.0f})},
                DrawItem{plane, mat,
                         glm::translate(glm::mat4(1.0f), {0.5f, 0.0f, -2.0f}) *
                             glm::scale(glm::mat4(1.0f), {0.04f, 0.5f, 1.0f})},
            };
            const auto singleAngled =
                makeCascadedShadowFrame(singleLarge, angledLight.direction, cam, 0.1f, 100.0f, sceneBounds);
            const auto csmAngled =
                makeCascadedShadowFrame(csmSettings, angledLight.direction, cam, 0.1f, 100.0f, sceneBounds);

            constexpr size_t center = 64u * 128u + 64u;
            const auto hdrSingle = captureHdr(
                renderer, FrameInput{extent, cam, angledNearDraws, angledLight, singleAngled, angledNearDraws});
            const auto hdrCsm = captureHdr(
                renderer, FrameInput{extent, cam, angledNearDraws, angledLight, csmAngled, angledNearDraws});
            ASSERT_GT(hdrSingle.size(), center);
            ASSERT_GT(hdrCsm.size(), center);
            // 3 级联第 0 级分辨率密度远高于单级大范围，中心阴影遮挡明显更清晰更暗
            EXPECT_LT(hdrCsm[center][0], 0.1f);
            EXPECT_LT(hdrCsm[center][0] + 0.1f, hdrSingle[center][0]);

            // 验证远景（z = -36 接收面、z = -34 投射体，落在第 2 级联层内）正确采样高层级联投下阴影
            const std::array farDraws{
                DrawItem{plane, mat,
                         glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, -36.0f}) *
                             glm::scale(glm::mat4(1.0f), {10.0f, 10.0f, 1.0f})},
                DrawItem{plane, mat,
                         glm::translate(glm::mat4(1.0f), {1.0f, 0.0f, -34.0f}) *
                             glm::scale(glm::mat4(1.0f), {1.0f, 1.0f, 1.0f})},
            };
            const auto hdrFarCsm =
                captureHdr(renderer, FrameInput{extent, cam, farDraws, angledLight, csmAngled, farDraws});
            EXPECT_LT(hdrFarCsm[center][0], 0.1f);

            // 验证 Alpha Mask 镂空在 3 级联下同样正确：左半透明（接收面受光），右半不透明（接收面被遮挡）
            const std::array maskCasters{
                DrawItem{plane, maskMat,
                         glm::translate(glm::mat4(1.0f), {0.5f, 0.0f, -2.0f}) *
                             glm::scale(glm::mat4(1.0f), {0.8f, 0.8f, 1.0f})},
            };
            const std::array maskReceivers{angledNearDraws[0]};
            const auto hdrMask = captureHdr(
                renderer, FrameInput{extent, cam, maskReceivers, angledLight, csmAngled, maskCasters});
            const size_t leftLit = 64u * 128u + 48u;
            const size_t rightDark = 64u * 128u + 80u;
            EXPECT_GT(hdrMask[leftLit][0], 0.5f);
            EXPECT_LT(hdrMask[rightDark][0], 0.1f);

            // 验证单级 <-> 3 级联切换及离屏目标尺寸变化在 drawSceneTarget 下零 Validation 错误
            for (const auto& [cascades, targetExt] :
                 {std::pair{1u, FrameExtent{96, 96}}, {3u, FrameExtent{96, 96}}, {3u, FrameExtent{160, 120}},
                  {1u, FrameExtent{160, 120}}})
            {
                csmSettings.cascadeCount = cascades;
                const auto frameShadow =
                    makeCascadedShadowFrame(csmSettings, angledLight.direction, cam, 0.1f, 100.0f, sceneBounds);
                window->pollEvents();
                ASSERT_TRUE(renderer.beginUiFrame());
                const auto output = renderer.prepareSceneTarget(targetExt);
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(
                    output, FrameInput{targetExt, cam, angledNearDraws, angledLight, frameShadow, angledNearDraws},
                    {0, 0, 0, 1});
            }
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, AcesFilmicAndSkyboxWithBlendAndFallbackUnderValidation)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);

            auto& manager = Resource::ResourceManager::instance();
            const auto skyVert =
                manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, "shaders/skybox.vert.spv");
            const auto skyFrag =
                manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, "shaders/skybox.frag.spv");
            ASSERT_TRUE(skyVert && skyFrag);

            const Resource::ShaderResource emptyShader({}, {});
            EXPECT_THROW(renderer.prepareSkyboxResources(emptyShader, *skyFrag.get<Resource::ShaderResource>()),
                         Error::Exception);
            renderer.prepareSkyboxResources(*skyVert.get<Resource::ShaderResource>(),
                                            *skyFrag.get<Resource::ShaderResource>());
            EXPECT_THROW(renderer.prepareSkyboxResources(*skyVert.get<Resource::ShaderResource>(),
                                                         *skyFrag.get<Resource::ShaderResource>()),
                         Error::Exception);

            const auto encodeSrgb8 = [](float linear) {
                const float clamped = std::clamp(linear, 0.0f, 1.0f);
                const float encoded =
                    clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
                return encoded * 255.0f;
            };
            const auto aces = [](float x) {
                return std::clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
            };
            const auto perspectiveCamera = [](glm::vec3 target, FrameExtent ext) {
                const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f), target, glm::vec3(0.0f, 1.0f, 0.0f));
                glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(60.0f), float(ext.width) / ext.height, 0.1f, 50.0f);
                proj[1][1] *= -1.0f;
                return CameraMatrices{view, proj};
            };
            const auto renderLdr = [&](FrameExtent ext, const FrameInput& input, std::array<float, 4> clear) {
                window->pollEvents();
                EXPECT_TRUE(renderer.beginUiFrame());
                const auto output = renderer.prepareSceneTarget(ext);
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(output, input, clear);
                std::vector<uint8_t> pixels;
                Detail::RendererReviewAccess::withSceneOutput(
                    renderer, output, [&](const auto& device, const auto& image) {
                        pixels = TextureGpuReview::captureMip(device, image, 0, {ext.width, ext.height});
                    });
                return pixels;
            };

            const FrameExtent ext{64, 64};
            const std::array<float, 4> clearColor{0.18f, 0.34f, 0.52f, 1.0f};
            const CameraMatrices camNegZ = perspectiveCamera({0.0f, 0.0f, -1.0f}, ext);

            // 1. IBL 未就绪时启用 skyboxEnabled 不应崩溃或报错，应退回到 clearColor 背景。
            {
                FrameInput fallbackInput{ext, camNegZ, {}};
                fallbackInput.display.skyboxEnabled = true;
                const auto px = renderLdr(ext, fallbackInput, clearColor);
                const size_t center = (32u * ext.width + 32u) * 4u;
                EXPECT_NEAR(px[center + 0], clearColor[0] * 255.0f, 1.5f);
                EXPECT_NEAR(px[center + 1], clearColor[1] * 255.0f, 1.5f);
                EXPECT_NEAR(px[center + 2], clearColor[2] * 255.0f, 1.5f);

                const auto hdrFallback = captureHdr(renderer, fallbackInput);
                EXPECT_FLOAT_EQ(hdrFallback[32u * ext.width + 32u][3], 0.0f);
            }

            // 2. 构造有明显方向颜色差异的合成立方体贴图 (+X红, -X绿, +Y蓝, -Y黄, +Z青, -Z橙)。
            const auto makeSolidFace = [](std::vector<uint16_t>& dst, uint32_t pixelsPerFace,
                                          std::array<float, 4> rgba) {
                for (uint32_t i = 0; i < pixelsPerFace; ++i)
                {
                    dst.push_back(glm::packHalf1x16(rgba[0]));
                    dst.push_back(glm::packHalf1x16(rgba[1]));
                    dst.push_back(glm::packHalf1x16(rgba[2]));
                    dst.push_back(glm::packHalf1x16(rgba[3]));
                }
            };
            std::vector<uint16_t> prePixels;
            const std::array<std::array<float, 4>, 6> faceColors{{
                {1.2f, 0.06f, 0.06f, 1.0f},  // +X 红
                {0.06f, 1.2f, 0.06f, 1.0f},  // -X 绿
                {0.06f, 0.12f, 1.1f, 1.0f},  // +Y 蓝
                {1.0f, 1.0f, 0.06f, 1.0f},   // -Y 黄
                {0.06f, 1.0f, 1.0f, 1.0f},   // +Z 青
                {1.4f, 0.55f, 0.08f, 1.0f},  // -Z 橙
            }};
            for (const auto& color : faceColors)
                makeSolidFace(prePixels, 4u, color);
            std::vector<uint16_t> irrPixels;
            for (int f = 0; f < 6; ++f)
                makeSolidFace(irrPixels, 1u, {0.1f, 0.1f, 0.1f, 1.0f});
            const std::vector<uint16_t> lutPixels{glm::packHalf1x16(1.0f), glm::packHalf1x16(0.0f),
                                                  glm::packHalf1x16(0.0f), glm::packHalf1x16(1.0f)};

            const auto irrTex = renderer.createTexture(Resource::FloatTextureResource(
                {}, Resource::FloatTextureDimension::CubeMap, {Resource::FloatTextureMip{1, 1, std::move(irrPixels)}}));
            const auto preTex = renderer.createTexture(Resource::FloatTextureResource(
                {}, Resource::FloatTextureDimension::CubeMap, {Resource::FloatTextureMip{2, 2, std::move(prePixels)}}));
            const auto lutTex = renderer.createTexture(Resource::FloatTextureResource(
                {}, Resource::FloatTextureDimension::Texture2D, {Resource::FloatTextureMip{1, 1, lutPixels}}));
            renderer.prepareIblResources(irrTex, preTex, lutTex);

            // 3. 验证相机旋转时天空盒方向颜色随之变化。
            {
                FrameInput skyNegZ{ext, camNegZ, {}};
                skyNegZ.display.skyboxEnabled = true;
                const auto hdrNegZ = captureHdr(renderer, skyNegZ);
                const auto& cNegZ = hdrNegZ[32u * ext.width + 32u];
                EXPECT_NEAR(cNegZ[0], 1.4f, 0.05f);
                EXPECT_NEAR(cNegZ[1], 0.55f, 0.05f);
                EXPECT_NEAR(cNegZ[2], 0.08f, 0.05f);
                EXPECT_FLOAT_EQ(cNegZ[3], 1.0f);

                FrameInput skyPosX{ext, perspectiveCamera({1.0f, 0.0f, 0.0f}, ext), {}};
                skyPosX.display.skyboxEnabled = true;
                const auto hdrPosX = captureHdr(renderer, skyPosX);
                const auto& cPosX = hdrPosX[32u * ext.width + 32u];
                EXPECT_GT(cPosX[0], 1.0f);
                EXPECT_LT(cPosX[1], 0.15f);

                FrameInput skyNegX{ext, perspectiveCamera({-1.0f, 0.0f, 0.0f}, ext), {}};
                skyNegX.display.skyboxEnabled = true;
                const auto hdrNegX = captureHdr(renderer, skyNegX);
                const auto& cNegX = hdrNegX[32u * ext.width + 32u];
                EXPECT_GT(cNegX[1], 1.0f);
                EXPECT_LT(cNegX[0], 0.15f);
            }

            // 4. 验证近处 Opaque 不被天空盒覆盖，半透明 Blend 在天空盒之上混合。
            const auto plane = createLitPlane(renderer);
            const auto white = renderer.createTexture(Resource::TextureResource({}, 1, 1, {255, 255, 255, 255}));
            MaterialDesc opaqueBlueDesc{white, {0.0f, 0.0f, 0.0f}};
            opaqueBlueDesc.shading = ShadingMode::Pbr;
            opaqueBlueDesc.emissiveTexture = white;
            opaqueBlueDesc.emissiveFactor = {0.04f, 0.06f, 0.85f};
            const auto opaqueBlueMat = renderer.createMaterial(opaqueBlueDesc);

            MaterialDesc blendCyanDesc{white, {0.0f, 0.0f, 0.0f}};
            blendCyanDesc.shading = ShadingMode::Pbr;
            blendCyanDesc.alphaMode = AlphaMode::Blend;
            blendCyanDesc.doubleSided = true;
            blendCyanDesc.baseAlpha = 0.5f;
            blendCyanDesc.emissiveTexture = white;
            blendCyanDesc.emissiveFactor = {0.0f, 0.2f, 1.0f};
            const auto blendBlueMat = renderer.createMaterial(blendCyanDesc);

            {
                const std::array draws{
                    DrawItem{plane, opaqueBlueMat,
                             glm::translate(glm::mat4(1.0f), {-0.6f, 0.0f, -2.5f}) *
                                 glm::scale(glm::mat4(1.0f), {0.45f, 0.45f, 1.0f})},
                    DrawItem{plane, blendBlueMat,
                             glm::translate(glm::mat4(1.0f), {0.6f, 0.0f, -2.5f}) *
                                 glm::scale(glm::mat4(1.0f), {0.45f, 0.45f, 1.0f})},
                };
                FrameInput sceneInput{ext, camNegZ, draws, Lighting{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 0.0f, 0.0f}};
                sceneInput.display.skyboxEnabled = true;
                const auto hdr = captureHdr(renderer, sceneInput);
                const auto& leftOpaque = hdr[32u * ext.width + 18u];
                const auto& centerSky = hdr[32u * ext.width + 32u];
                const auto& rightBlend = hdr[32u * ext.width + 46u];
                // 左侧近距不透明蓝块遮挡橙色天空盒
                EXPECT_LT(leftOpaque[0], 0.15f);
                EXPECT_GT(leftOpaque[2], 0.7f);
                // 中间暴露天空盒 (-Z 橙色)
                EXPECT_NEAR(centerSky[0], 1.4f, 0.05f);
                EXPECT_NEAR(centerSky[1], 0.55f, 0.05f);
                // 右侧半透明蓝块在天空盒上方混合：保留天空盒约一半橙色并叠加蓝色
                EXPECT_GT(rightBlend[0], 0.45f);
                EXPECT_LT(rightBlend[0], 1.1f);
                EXPECT_GT(rightBlend[2], 0.45f);
            }

            // 5. 验证 ACES Filmic 色调映射曲线数值、高光区分能力及曝光响应。
            {
                MaterialDesc leftHiDesc{white, {0.06f, 0.03f, 0.01f}};
                leftHiDesc.shading = ShadingMode::Lambert;
                const auto leftMat = renderer.createMaterial(leftHiDesc);

                MaterialDesc rightHiDesc{white, {0.5f, 0.35f, 0.15f}};
                rightHiDesc.shading = ShadingMode::Lambert;
                const auto rightMat = renderer.createMaterial(rightHiDesc);

                const std::array draws{
                    DrawItem{plane, leftMat,
                             glm::translate(glm::mat4(1.0f), {-0.6f, 0.0f, -2.5f}) *
                                 glm::scale(glm::mat4(1.0f), {0.45f, 0.45f, 1.0f})},
                    DrawItem{plane, rightMat,
                             glm::translate(glm::mat4(1.0f), {0.6f, 0.0f, -2.5f}) *
                                 glm::scale(glm::mat4(1.0f), {0.45f, 0.45f, 1.0f})},
                };
                const Lighting acesLight{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 10.0f, 0.0f};
                FrameInput acesInput{ext, camNegZ, draws, acesLight};
                acesInput.display = {ToneMappingMode::AcesFilmic, 0.0f, false, false};
                const auto aces0 = renderLdr(ext, acesInput, {0, 0, 0, 1});
                const size_t lOff = (32u * ext.width + 18u) * 4u;
                const size_t rOff = (32u * ext.width + 46u) * 4u;
                EXPECT_NEAR(aces0[lOff + 0], encodeSrgb8(aces(0.6f)), 2.5f);
                EXPECT_NEAR(aces0[lOff + 1], encodeSrgb8(aces(0.3f)), 2.5f);
                EXPECT_NEAR(aces0[lOff + 2], encodeSrgb8(aces(0.1f)), 2.5f);
                EXPECT_NEAR(aces0[rOff + 0], encodeSrgb8(aces(5.0f)), 2.5f);
                EXPECT_GT(aces0[rOff + 0], aces0[lOff + 0] + 30);

                acesInput.display.exposureEv = 1.0f;
                const auto acesPlus = renderLdr(ext, acesInput, {0, 0, 0, 1});
                EXPECT_NEAR(acesPlus[lOff + 0], encodeSrgb8(aces(1.2f)), 2.5f);

                acesInput.display.exposureEv = -1.0f;
                const auto acesMinus = renderLdr(ext, acesInput, {0, 0, 0, 1});
                EXPECT_NEAR(acesMinus[lOff + 0], encodeSrgb8(aces(0.3f)), 2.5f);
            }
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, HdrBloomHaloThresholdsToggleAndTinyResizeUnderValidation)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);

            auto& manager = Resource::ResourceManager::instance();
            const auto bloomPre = manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER,
                                                                         "shaders/bloom_prefilter.frag.spv");
            const auto bloomDown = manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER,
                                                                          "shaders/bloom_downsample.frag.spv");
            const auto bloomUp = manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER,
                                                                        "shaders/bloom_upsample.frag.spv");
            const auto fxaaFrag =
                manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, "shaders/fxaa.frag.spv");
            ASSERT_TRUE(bloomPre && bloomDown && bloomUp && fxaaFrag);

            renderer.prepareFxaaResources(*fxaaFrag.get<Resource::ShaderResource>());
            const Resource::ShaderResource emptyShader({}, {});
            EXPECT_THROW(renderer.prepareBloomResources(emptyShader, *bloomDown.get<Resource::ShaderResource>(),
                                                        *bloomUp.get<Resource::ShaderResource>()),
                         Error::Exception);
            renderer.prepareBloomResources(*bloomPre.get<Resource::ShaderResource>(),
                                           *bloomDown.get<Resource::ShaderResource>(),
                                           *bloomUp.get<Resource::ShaderResource>());
            EXPECT_THROW(renderer.prepareBloomResources(*bloomPre.get<Resource::ShaderResource>(),
                                                        *bloomDown.get<Resource::ShaderResource>(),
                                                        *bloomUp.get<Resource::ShaderResource>()),
                         Error::Exception);

            const auto white = renderer.createTexture(Resource::TextureResource({}, 1, 1, {255, 255, 255, 255}));
            const auto plane = createLitPlane(renderer);
            MaterialDesc hotDesc{white, {1.0f, 0.35f, 0.05f}};
            hotDesc.shading = ShadingMode::Lambert;
            const auto hotMat = renderer.createMaterial(hotDesc);
            const std::array hotDraws{
                DrawItem{plane, hotMat, glm::scale(glm::mat4(1.0f), {0.18f, 0.18f, 1.0f})},
            };
            const Lighting hotLight{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 8.0f, 0.0f};

            const auto orthoCam = [](FrameExtent ext) {
                const float aspect = float(ext.width) / ext.height;
                glm::mat4 proj = glm::orthoRH_ZO(-aspect, aspect, -1.0f, 1.0f, 0.1f, 10.0f);
                proj[1][1] *= -1.0f;
                return CameraMatrices{glm::lookAtRH(glm::vec3(0, 0, 3), glm::vec3(0), glm::vec3(0, 1, 0)), proj};
            };
            const auto renderLdr = [&](FrameExtent ext, const DisplaySettings& display, bool fxaa = false) {
                window->pollEvents();
                EXPECT_TRUE(renderer.beginUiFrame());
                const auto output = renderer.prepareSceneTarget(ext, fxaa);
                renderer.imgui().endFrame();
                FrameInput input{ext, orthoCam(ext), hotDraws, hotLight};
                input.display = display;
                renderer.drawSceneTarget(output, input, {0.0f, 0.0f, 0.0f, 1.0f});
                std::vector<uint8_t> pixels;
                Detail::RendererReviewAccess::withSceneOutput(
                    renderer, output, [&](const auto& device, const auto& image) {
                        pixels = TextureGpuReview::captureMip(device, image, 0, {ext.width, ext.height});
                    });
                return pixels;
            };

            const FrameExtent ext{128, 128};
            const auto pixelR = [&](const std::vector<uint8_t>& px, uint32_t x, uint32_t y) {
                return px[(size_t(y) * ext.width + x) * 4u + 0u];
            };
            const auto pixelG = [&](const std::vector<uint8_t>& px, uint32_t x, uint32_t y) {
                return px[(size_t(y) * ext.width + x) * 4u + 1u];
            };

            // 1. 关闭 Bloom 时，亮核四周黑背景应完全为 0，不受 bloomIntensity 影响。
            const auto bloomOffA =
                renderLdr(ext, {ToneMappingMode::AcesFilmic, 0.0f, false, false, 1.0f, 0.0f});
            const auto bloomOffB =
                renderLdr(ext, {ToneMappingMode::AcesFilmic, 0.0f, false, false, 1.0f, 2.0f});
            EXPECT_EQ(bloomOffA, bloomOffB);
            EXPECT_EQ(pixelR(bloomOffA, 64, 38), 0u);
            EXPECT_EQ(pixelR(bloomOffA, 38, 64), 0u);

            // 2. 开启 Bloom 且阈值 <= 高亮值时，四周黑背景产生近似对称并随距离衰减的光晕。
            const auto bloomOn =
                renderLdr(ext, {ToneMappingMode::AcesFilmic, 0.0f, false, true, 1.0f, 1.0f});
            const uint8_t nearTopR = pixelR(bloomOn, 64, 42);
            const uint8_t nearBottomR = pixelR(bloomOn, 64, 86);
            const uint8_t nearLeftR = pixelR(bloomOn, 42, 64);
            const uint8_t nearRightR = pixelR(bloomOn, 86, 64);
            const uint8_t farTopR = pixelR(bloomOn, 64, 24);
            EXPECT_GT(nearTopR, 25u);
            EXPECT_GT(nearTopR, pixelG(bloomOn, 64, 42));
            EXPECT_NEAR(float(nearTopR), float(nearBottomR), 6.0f);
            EXPECT_NEAR(float(nearLeftR), float(nearRightR), 6.0f);
            EXPECT_GT(nearTopR, farTopR + 8u);
            EXPECT_GT(farTopR, 0u);

            // 3. 阈值升高超过高亮辐射度时应抑制光晕；强度放大时同位置光晕单调递增。
            const auto highThreshold =
                renderLdr(ext, {ToneMappingMode::AcesFilmic, 0.0f, false, true, 20.0f, 1.0f});
            EXPECT_EQ(pixelR(highThreshold, 64, 42), 0u);

            const auto lowIntensity =
                renderLdr(ext, {ToneMappingMode::AcesFilmic, 0.0f, false, true, 1.0f, 0.4f});
            const auto highIntensity =
                renderLdr(ext, {ToneMappingMode::AcesFilmic, 0.0f, false, true, 1.0f, 2.5f});
            EXPECT_GT(pixelR(highIntensity, 64, 42), pixelR(lowIntensity, 64, 42));

            // 4. FXAA 联用与开关切换 + 极小尺寸缩放 ({4,4}, {2,2}) 在同步验证下应无布局/屏障错误。
            for (const auto& [targetExt, enableBloom, fxaa] :
                 {std::tuple{FrameExtent{128, 128}, true, true}, std::tuple{FrameExtent{128, 128}, false, false},
                  std::tuple{FrameExtent{128, 128}, true, false}, std::tuple{FrameExtent{4, 4}, true, false},
                  std::tuple{FrameExtent{2, 2}, true, true}, std::tuple{FrameExtent{64, 48}, true, false}})
            {
                const auto px = renderLdr(
                    targetExt, {ToneMappingMode::AcesFilmic, 0.0f, false, enableBloom, 1.0f, 0.8f}, fxaa);
                EXPECT_EQ(px.size(), size_t(targetExt.width) * targetExt.height * 4u);
            }
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, LocalPointAndSpotLightsPbrAndLambertAttenuationUnderValidation)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);

            const auto white = renderer.createTexture(Resource::TextureResource({}, 1, 1, {255, 255, 255, 255}));
            const auto plane = createLitPlane(renderer);
            const auto largePlane = DrawItem{plane, Material{}, glm::scale(glm::mat4(1.0f), {8.0f, 6.0f, 1.0f})};

            MaterialDesc lambertDesc{white, {1.0f, 1.0f, 1.0f}};
            lambertDesc.shading = ShadingMode::Lambert;
            const auto lambertMat = renderer.createMaterial(lambertDesc);

            MaterialDesc pbrDesc{white, {0.8f, 0.8f, 0.8f}};
            pbrDesc.shading = ShadingMode::Pbr;
            pbrDesc.roughnessMode = MaterialParameterMode::FixedValue;
            pbrDesc.roughness = 0.55f;
            pbrDesc.metallicMode = MaterialParameterMode::FixedValue;
            pbrDesc.metallic = 0.1f;
            const auto pbrMat = renderer.createMaterial(pbrDesc);

            MaterialDesc unlitDesc{white, {0.2f, 0.3f, 0.4f}};
            unlitDesc.shading = ShadingMode::Unlit;
            const auto unlitMat = renderer.createMaterial(unlitDesc);

            const FrameExtent ext{160, 120};
            // 世界范围 [-4, 4] x [-3, 3] 精确一一映射至 160x120（每单位 20 像素，中心 x=80, y=60）。
            const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            glm::mat4 proj = glm::orthoRH_ZO(-4.0f, 4.0f, -3.0f, 3.0f, 0.1f, 20.0f);
            proj[1][1] *= -1.0f;
            const CameraMatrices cam{view, proj};

            const auto sampleWorld = [&](const std::vector<std::array<float, 4>>& hdr, float wx, float wy) {
                const uint32_t px = std::min(ext.width - 1u, static_cast<uint32_t>(std::lround((wx + 4.0f) * 20.0f)));
                const uint32_t py = std::min(ext.height - 1u, static_cast<uint32_t>(std::lround((3.0f - wy) * 20.0f)));
                return hdr[size_t(py) * ext.width + px];
            };
            const auto renderMatHdr = [&](const Material& mat, const Lighting& lighting) {
                const std::array draws{DrawItem{largePlane.mesh, mat, largePlane.model}};
                return captureHdr(renderer, FrameInput{ext, cam, draws, lighting});
            };

            Lighting baseLight;
            baseLight.intensity = 0.0f;
            baseLight.ambient = 0.0f;

            // 1. 基线：关闭方向光、环境光与局部光源时，Lambert 与 PBR 均为纯黑。
            EXPECT_FLOAT_EQ(sampleWorld(renderMatHdr(lambertMat, baseLight), 0.0f, 0.0f)[0], 0.0f);
            EXPECT_FLOAT_EQ(sampleWorld(renderMatHdr(pbrMat, baseLight), 0.0f, 0.0f)[0], 0.0f);

            // 2. 点光源距离衰减：中心 (dist=1.5) 符合 KHR_lights_punctual 解析解，近亮远暗，超 range(3.0) 为零。
            LocalLight point;
            point.type = LocalLightType::Point;
            point.position = {0.0f, 0.0f, 1.5f};
            point.color = {1.0f, 0.5f, 0.25f};
            point.intensity = 8.0f;
            point.range = 3.0f;
            Lighting pointLighting = baseLight;
            pointLighting.localLights = {point};

            const float ratioCenter = 1.5f / 3.0f;
            const float winCenter = 1.0f - std::pow(ratioCenter, 4.0f);
            const float expectedCenterAtten = (winCenter * winCenter) / (1.5f * 1.5f);
            const float expectedCenterRed = point.color.r * point.intensity * expectedCenterAtten;

            for (const auto& [mat, isLambert] : {std::pair{lambertMat, true}, std::pair{pbrMat, false}})
            {
                const auto hdr = renderMatHdr(mat, pointLighting);
                const auto cCenter = sampleWorld(hdr, 0.0f, 0.0f);
                const auto cMid = sampleWorld(hdr, 1.5f, 0.0f);
                const auto cOut = sampleWorld(hdr, 3.2f, 0.0f);
                EXPECT_GT(cCenter[0], cMid[0] * 3.0f);
                EXPECT_GT(cMid[0], 0.01f);
                EXPECT_FLOAT_EQ(cOut[0], 0.0f);
                EXPECT_FLOAT_EQ(cOut[1], 0.0f);
                EXPECT_FLOAT_EQ(cOut[2], 0.0f);
                EXPECT_NEAR(cCenter[1] / cCenter[0], 0.5f, 0.02f);
                if (isLambert)
                    EXPECT_NEAR(cCenter[0], expectedCenterRed, 0.05f);
            }

            // 3. 聚光灯内外锥角平滑过渡：内锥满照度，过渡带平滑衰减，外锥之外严格归零。
            LocalLight spot;
            spot.type = LocalLightType::Spot;
            spot.position = {0.0f, 0.0f, 2.0f};
            spot.direction = {0.0f, 0.0f, -1.0f};
            spot.color = {0.3f, 0.9f, 1.0f};
            spot.intensity = 10.0f;
            spot.range = 10.0f;
            spot.innerConeAngle = 0.25f;  // r ~= 0.51
            spot.outerConeAngle = 0.65f;  // r ~= 1.52
            Lighting spotLighting = baseLight;
            spotLighting.localLights = {spot};

            // 对比同位置同参数的 Point 光源，验证内锥以内与过渡带的 spot 因子。
            LocalLight spotAsPoint = spot;
            spotAsPoint.type = LocalLightType::Point;
            Lighting spotAsPointLighting = baseLight;
            spotAsPointLighting.localLights = {spotAsPoint};

            for (const auto& mat : {lambertMat, pbrMat})
            {
                const auto hdrSpot = renderMatHdr(mat, spotLighting);
                const auto hdrRefPoint = renderMatHdr(mat, spotAsPointLighting);
                const auto cInnerSpot = sampleWorld(hdrSpot, 0.0f, 0.0f);
                const auto cInnerRef = sampleWorld(hdrRefPoint, 0.0f, 0.0f);
                EXPECT_NEAR(cInnerSpot[1], cInnerRef[1], 1e-3f);

                const auto cMidSpot = sampleWorld(hdrSpot, 0.9f, 0.0f);
                const auto cMidRef = sampleWorld(hdrRefPoint, 0.9f, 0.0f);
                EXPECT_GT(cMidSpot[1], 0.05f);
                EXPECT_LT(cMidSpot[1], cMidRef[1] * 0.85f);

                const auto cOutsideSpot = sampleWorld(hdrSpot, 2.0f, 0.0f);
                const auto cOutsideRef = sampleWorld(hdrRefPoint, 2.0f, 0.0f);
                EXPECT_GT(cOutsideRef[1], 0.05f);
                EXPECT_FLOAT_EQ(cOutsideSpot[1], 0.0f);
            }

            // 4. 多光源线性累加 + disabled 光源跳过 + Unlit 不受局部光源影响。
            LocalLight disabledGreen = point;
            disabledGreen.enabled = false;
            disabledGreen.color = {0.0f, 1.0f, 0.0f};
            disabledGreen.intensity = 50.0f;

            LocalLight redPoint = point;
            redPoint.color = {1.0f, 0.0f, 0.0f};
            LocalLight blueSpot = spot;
            blueSpot.color = {0.0f, 0.0f, 1.0f};

            Lighting combinedLighting = baseLight;
            combinedLighting.localLights = {redPoint, disabledGreen, blueSpot};
            Lighting redOnly = baseLight;
            redOnly.localLights = {redPoint};
            Lighting blueOnly = baseLight;
            blueOnly.localLights = {blueSpot};

            for (const auto& mat : {lambertMat, pbrMat})
            {
                const auto hdrComb = renderMatHdr(mat, combinedLighting);
                const auto hdrRed = renderMatHdr(mat, redOnly);
                const auto hdrBlue = renderMatHdr(mat, blueOnly);
                const auto cComb = sampleWorld(hdrComb, 0.0f, 0.0f);
                const auto cRed = sampleWorld(hdrRed, 0.0f, 0.0f);
                const auto cBlue = sampleWorld(hdrBlue, 0.0f, 0.0f);
                EXPECT_NEAR(cComb[0], cRed[0] + cBlue[0], 0.02f);
                EXPECT_FLOAT_EQ(cComb[1], 0.0f);  // disabled 绿灯未生效
                EXPECT_NEAR(cComb[2], cRed[2] + cBlue[2], 0.02f);
            }

            const auto hdrUnlit = renderMatHdr(unlitMat, combinedLighting);
            const auto cUnlit = sampleWorld(hdrUnlit, 0.0f, 0.0f);
            EXPECT_NEAR(cUnlit[0], 0.2f, 0.01f);
            EXPECT_NEAR(cUnlit[1], 0.3f, 0.01f);
            EXPECT_NEAR(cUnlit[2], 0.4f, 0.01f);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, DebugLinesDepthOcclusionAndDynamicToggleUnderValidation)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);

            auto& manager = Resource::ResourceManager::instance();
            const auto lineVert =
                manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, "shaders/debug_line.vert.spv");
            const auto lineFrag =
                manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, "shaders/debug_line.frag.spv");
            ASSERT_TRUE(lineVert && lineFrag);

            const Resource::ShaderResource emptyShader({}, {});
            EXPECT_THROW(renderer.prepareDebugLineResources(emptyShader, *lineFrag.get<Resource::ShaderResource>()),
                         Error::Exception);
            const auto white = renderer.createTexture(Resource::TextureResource({}, 1, 1, {255, 255, 255, 255}));
            const auto plane = createLitPlane(renderer);
            MaterialDesc opaqueRedDesc{white, {0.0f, 0.0f, 0.0f}};
            opaqueRedDesc.shading = ShadingMode::Pbr;
            opaqueRedDesc.emissiveTexture = white;
            opaqueRedDesc.emissiveFactor = {0.8f, 0.05f, 0.05f};
            const auto redMat = renderer.createMaterial(opaqueRedDesc);

            const FrameExtent ext{64, 64};
            glm::mat4 proj = glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 10.0f);
            proj[1][1] *= -1.0f;
            const CameraMatrices cam{glm::lookAtRH(glm::vec3(0, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0)), proj};
            const std::array draws{
                DrawItem{plane, redMat,
                         glm::translate(glm::mat4(1.0f), {-1.0f, 0.0f, -2.0f}) *
                             glm::scale(glm::mat4(1.0f), {2.0f, 2.0f, 1.0f})},
            };
            // 未准备 DebugLine 资源时提交非空 debugLines 应抛出 RENDER。
            const std::array singleLine{DebugLine{{-1.8f, 0.0f, -1.0f}, {1.8f, 0.0f, -1.0f}, {0.1f, 0.9f, 0.1f}, 1.0f}};
            FrameInput unreadyInput{ext, cam, draws};
            unreadyInput.debugLines = singleLine;
            EXPECT_THROW((void)captureHdr(renderer, unreadyInput), Error::Exception);

            renderer.prepareDebugLineResources(*lineVert.get<Resource::ShaderResource>(),
                                               *lineFrag.get<Resource::ShaderResource>());
            EXPECT_THROW(renderer.prepareDebugLineResources(*lineVert.get<Resource::ShaderResource>(),
                                                            *lineFrag.get<Resource::ShaderResource>()),
                         Error::Exception);

            // 前景绿线 (z=-1) 在红色方块 (z=-2) 前面绘制；后景蓝线 (z=-3) 左侧被红块遮挡，右侧无遮挡可见。
            constexpr float yTop = 2.0f - 24.5f / 16.0f;
            constexpr float yBot = 2.0f - 40.5f / 16.0f;
            const std::array depthLines{
                DebugLine{{-1.8f, yTop, -1.0f}, {1.8f, yTop, -1.0f}, {0.1f, 0.9f, 0.1f}, 1.0f},
                DebugLine{{-1.8f, yBot, -3.0f}, {1.8f, yBot, -3.0f}, {0.1f, 0.2f, 0.95f}, 1.0f},
            };
            FrameInput lineInput{ext, cam, draws};
            lineInput.debugLines = depthLines;
            const auto hdr = captureHdr(renderer, lineInput);
            const auto samplePixel = [&](uint32_t px, uint32_t py) {
                return hdr[size_t(py) * ext.width + px];
            };
            // 第 24 行（yTop）绿线在红块前面 (x=16 左侧和 x=48 右侧均显绿)
            EXPECT_GT(samplePixel(16u, 24u)[1], 0.7f);
            EXPECT_GT(samplePixel(48u, 24u)[1], 0.7f);
            // 第 40 行（yBot）蓝线左侧被 z=-2 红块遮挡（保持红），右侧穿过背景（显蓝）
            const auto occludedPx = samplePixel(16u, 40u);
            EXPECT_GT(occludedPx[0], 0.6f);
            EXPECT_LT(occludedPx[2], 0.15f);
            EXPECT_GT(samplePixel(48u, 40u)[2], 0.7f);

            // 在 drawSceneTarget 中空线段 <-> 多线段切换与视口尺寸变化及截图回读保持零 Validation 错误。
            for (const auto& [targetExt, lines] :
                 {std::pair{FrameExtent{64, 64}, std::span<const DebugLine>{depthLines}},
                  std::pair{FrameExtent{64, 64}, std::span<const DebugLine>{}},
                  std::pair{FrameExtent{4, 4}, std::span<const DebugLine>{depthLines}},
                  std::pair{FrameExtent{96, 64}, std::span<const DebugLine>{depthLines}}})
            {
                window->pollEvents();
                ASSERT_TRUE(renderer.beginUiFrame());
                const auto output = renderer.prepareSceneTarget(targetExt);
                renderer.imgui().endFrame();
                FrameInput targetInput{targetExt, cam, draws};
                targetInput.debugLines = lines;
                renderer.drawSceneTarget(output, targetInput, {0, 0, 0, 1});
            }
            const auto captured = renderer.captureLastSceneViewport();
            ASSERT_TRUE(captured);
            EXPECT_EQ(captured->extent, (FrameExtent{96u, 64u}));
            EXPECT_EQ(captured->rgba8.size(), 96u * 64u * 4u);
        }
        expectCleanValidation();
    }
}  // namespace
