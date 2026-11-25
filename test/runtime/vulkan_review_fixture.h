#pragma once

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


namespace Incubator::Test
{
    using namespace Incubator;
    using namespace Incubator::Render;
    using namespace Incubator::Render::Vulkan;

    inline glm::vec3 linearFromSrgb(glm::vec3 v)
    {
        for (int c = 0; c < 3; ++c)
            v[c] = v[c] <= 0.04045f ? v[c] / 12.92f : std::pow((v[c] + 0.055f) / 1.055f, 2.4f);
        return v;
    }

    inline void expectLinearPixel(std::span<const uint8_t> pixels, glm::vec3 linear, uint32_t x = 192, uint32_t y = 128,
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

    inline std::vector<const char*> requiredExtensions()
    {
        return GlfwSurface::requiredInstanceExtensions();
    }

    inline size_t countOccurrences(std::string_view messages, std::string_view value)
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
    inline Demo::SceneAssetPaths referenceSceneAssetPaths()
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

    inline void expectReleased(Demo::SceneAssetPaths paths)
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

    inline void releaseCpuAssets(SceneAssets& assets)
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
}  // namespace Incubator::Test
