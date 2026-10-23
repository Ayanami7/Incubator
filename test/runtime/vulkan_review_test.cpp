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
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#include "support/render_demo/scene_assets.h"
#include "support/render_demo/scene_controls.h"
#include "support/render_demo/scene_frame.h"
#include "support/render_demo/scene_mesh.h"
#include "examples/demo/validation_mode.h"
#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/platform/window/window.h"
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
#include "support/texture_gpu_review.h"
#include "support/window_capture.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Render;
    using namespace Incubator::Render::Vulkan;

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
        bool destroyed = false;
        bool routesClearedBeforeDestroy = false;
        bool cpuReleasedBeforeDestroy = false;
    };

    class AuditedFileIO : public FileIO::FileIO
    {
    public:
        explicit AuditedFileIO(FileAudit& audit) : audit_(audit), inner_(Incubator::FileIO::createPlatformFileIO()) {}
        ~AuditedFileIO()
        {
            audit_.destroyed = true;
            audit_.routesClearedBeforeDestroy =
                !Resource::ResourceManager::instance().hasLoader(Resource::ResourceType::TEXTURE) &&
                !Resource::ResourceManager::instance().hasLoader(Resource::ResourceType::SHADER);
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

    TEST(RenderFrameInputTest, ApplicationOwnsTransformsAndFinalProjection)
    {
        const auto wide = Demo::makeSceneFrame({800, 400}, {});
        const auto square = Demo::makeSceneFrame({400, 400}, {});
        EXPECT_EQ(Demo::makeSceneModel({}), glm::mat4(1.0f));
        Demo::SceneTransform transform;
        transform.position = {1.0f, 2.0f, 3.0f};
        transform.rotationDegrees.z = 90.0f;
        transform.scale = {2.0f, 3.0f, 4.0f};
        const auto model = Demo::makeSceneModel(transform);
        const auto point = model * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        EXPECT_NEAR(point.x, 1.0f, 0.00001f);
        EXPECT_NEAR(point.y, 4.0f, 0.00001f);
        EXPECT_NEAR(point.z, 3.0f, 0.00001f);
        EXPECT_EQ(Demo::makeSceneModel(transform), model);
        transform.rotationDegrees = {90, 90, 90};
        const auto rotated = Demo::makeSceneModel(transform) * glm::vec4(1, 2, 3, 1);
        EXPECT_NEAR(rotated.x, 13, 0.00001f);
        EXPECT_NEAR(rotated.y, 8, 0.00001f);
        EXPECT_NEAR(rotated.z, 1, 0.00001f);
        EXPECT_NEAR(wide.camera.projection[0][0] * 2.0f, square.camera.projection[0][0], 0.00001f);
        EXPECT_EQ(wide.camera.projection[1][1], square.camera.projection[1][1]);
        EXPECT_LT(wide.camera.projection[1][1], 0.0f);
        const auto nearPoint = wide.camera.projection * glm::vec4(0.0f, 0.0f, -0.1f, 1.0f);
        const auto farPoint = wide.camera.projection * glm::vec4(0.0f, 0.0f, -50.0f, 1.0f);
        EXPECT_NEAR(nearPoint.z / nearPoint.w, 0.0f, 0.00001f);
        EXPECT_NEAR(farPoint.z / farPoint.w, 1.0f, 0.00001f);
    }

    TEST(RenderFrameInputTest, DemoControlsKeepModelsIndependentAndCameraFiniteWithinUiRanges)
    {
        Demo::SceneControls scene;
        const auto initial = Demo::makeSceneDraws(scene, {}, {}, {}, {});
        ASSERT_EQ(initial.size(), 2u);
        scene.cube.transform.position.x += 0.5f;
        scene.cube.transform.rotationDegrees.y += 30.0f;
        scene.cube.transform.scale.z = 2.0f;
        const auto changed = Demo::makeSceneDraws(scene, {}, {}, {}, {});
        EXPECT_NE(initial[0].model, changed[0].model);
        EXPECT_EQ(initial[1].model, changed[1].model);
        scene.cube.visible = false;
        EXPECT_EQ(Demo::makeSceneDraws(scene, {}, {}, {}, {}).size(), 1u);
        scene.plane.visible = false;
        EXPECT_TRUE(Demo::makeSceneDraws(scene, {}, {}, {}, {}).empty());
        scene.cube.visible = scene.plane.visible = true;
        EXPECT_EQ(Demo::makeSceneDraws(scene, {}, {}, {}, {}).size(), 2u);
        const auto defaultFrame = Demo::makeSceneFrame({1280, 720}, {}, scene.camera);
        for (const float pitch : {-80.0f, 80.0f})
            for (const float distance : {2.0f, 12.0f})
                for (const float fov : {20.0f, 80.0f})
                {
                    scene.camera.pitchDegrees = pitch;
                    scene.camera.distance = distance;
                    scene.camera.fieldOfViewDegrees = fov;
                    const auto frame = Demo::makeSceneFrame({1280, 720}, {}, scene.camera);
                    EXPECT_NE(frame.camera.view, defaultFrame.camera.view);
                    EXPECT_NE(frame.camera.projection, defaultFrame.camera.projection);
                    for (int column = 0; column < 4; ++column)
                        for (int row = 0; row < 4; ++row)
                        {
                            EXPECT_TRUE(std::isfinite(frame.camera.view[column][row]));
                            EXPECT_TRUE(std::isfinite(frame.camera.projection[column][row]));
                        }
                }
    }

    TEST(RenderColorTest, SurfaceSelectionHasStablePriorityAndExplicitFallback)
    {
        const VkSurfaceFormatKHR bgra{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        const VkSurfaceFormatKHR rgba{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        const std::array formats = {rgba, bgra};
        EXPECT_EQ(VulkanSwapchain::chooseSurfaceFormat(formats).format, bgra.format);
        EXPECT_EQ(VulkanSwapchain::chooseSurfaceFormat(std::span(&rgba, 1)).format, rgba.format);
        const VkSurfaceFormatKHR any{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        EXPECT_EQ(VulkanSwapchain::chooseSurfaceFormat(std::span(&any, 1)).format, bgra.format);
    }

    TEST(RenderColorTest, SurfaceSelectionRejectsSrgbAttachmentsHdrAndEmptyLists)
    {
        for (const VkSurfaceFormatKHR candidate :
             {VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
              VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT},
              VkSurfaceFormatKHR{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_HDR10_ST2084_EXT}})
        {
            try
            {
                VulkanSwapchain::chooseSurfaceFormat(std::span(&candidate, 1));
                FAIL() << "Unsupported surface was accepted";
            }
            catch (const Error::Exception& error)
            {
                EXPECT_EQ(error.code(), Error::Code::RENDER);
                EXPECT_NE(error.message().find(fmt::format("format {}", static_cast<int>(candidate.format))),
                          std::string::npos);
                EXPECT_NE(error.message().find(fmt::format("colorSpace {}", static_cast<int>(candidate.colorSpace))),
                          std::string::npos);
            }
        }
        EXPECT_THROW(VulkanSwapchain::chooseSurfaceFormat({}), Error::Exception);
    }

    TEST(RenderColorTest, ImageSupportRejectsControlledLimitsBeforeAnyAllocation)
    {
        VkPhysicalDeviceLimits limits{};
        limits.maxImageDimension2D = 4;
        VkFormatProperties format{};
        format.optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        VkImageFormatProperties image{};
        image.maxExtent = {4, 4, 1};
        image.maxMipLevels = image.maxArrayLayers = 1;
        image.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
        image.maxResourceSize = 64;
        EXPECT_NO_THROW(VulkanImage::validateRgba8Support({4, 4}, limits, format, image));
        EXPECT_THROW(VulkanImage::validateRgba8Support({5, 1}, limits, format, image), Error::Exception);
        EXPECT_THROW(VulkanImage::validateRgba8Support({1, 5}, limits, format, image), Error::Exception);
        EXPECT_THROW(VulkanImage::validateRgba8Support({0, 1}, limits, format, image), Error::Exception);
        for (auto feature : {VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, VK_FORMAT_FEATURE_TRANSFER_DST_BIT})
        {
            auto unavailable = format;
            unavailable.optimalTilingFeatures &= ~feature;
            EXPECT_THROW(VulkanImage::validateRgba8Support({2, 1}, limits, unavailable, image), Error::Exception);
        }
        for (int kind = 0; kind < 7; ++kind)
        {
            auto unavailable = image;
            if (kind == 0)
                unavailable.maxExtent.width = 1;
            if (kind == 1)
                unavailable.maxExtent.height = 0;
            if (kind == 2)
                unavailable.maxExtent.depth = 0;
            if (kind == 3)
                unavailable.maxMipLevels = 0;
            if (kind == 4)
                unavailable.maxArrayLayers = 0;
            if (kind == 5)
                unavailable.sampleCounts = 0;
            if (kind == 6)
                unavailable.maxResourceSize = 7;
            EXPECT_THROW(VulkanImage::validateRgba8Support({2, 1}, limits, format, unavailable), Error::Exception);
        }
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

    // Simulates the application's initial unavailable framebuffer without mocking Vulkan.
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

    TEST_F(VulkanReviewTest, TargetDeviceSupportsBaseline)
    {
        auto window = createWindow();
        {
            VulkanInstance instance(requiredExtensions(), ValidationMode::Required);
            ASSERT_TRUE(instance.validationEnabled());
            GlfwSurface surface(instance, *window);
            VulkanDevice device(instance, surface);
            const auto& properties = device.properties();
            EXPECT_NE(std::string_view(properties.deviceName).find("RTX 5070 Ti"), std::string_view::npos);
            EXPECT_GE(properties.apiVersion, VK_API_VERSION_1_3);
            VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{};
            maintenance.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
            VkPhysicalDeviceVulkan13Features features13{};
            features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            features13.pNext = &maintenance;
            VkPhysicalDeviceFeatures2 features{};
            features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features.pNext = &features13;
            vkGetPhysicalDeviceFeatures2(device.physicalDevice(), &features);
            EXPECT_TRUE(features13.dynamicRendering);
            EXPECT_TRUE(features13.synchronization2);
            EXPECT_TRUE(maintenance.swapchainMaintenance1);
            uint32_t extensionCount = 0;
            ASSERT_EQ(vkEnumerateDeviceExtensionProperties(device.physicalDevice(), nullptr, &extensionCount, nullptr),
                      VK_SUCCESS);
            std::vector<VkExtensionProperties> extensions(extensionCount);
            ASSERT_EQ(vkEnumerateDeviceExtensionProperties(device.physicalDevice(), nullptr, &extensionCount,
                                                           extensions.data()),
                      VK_SUCCESS);
            for (const char* required :
                 {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME})
                EXPECT_TRUE(std::any_of(extensions.begin(), extensions.end(), [required](const auto& extension) {
                    return std::string_view(extension.extensionName) == required;
                }));
            VkSurfaceCapabilitiesKHR capabilities{};
            ASSERT_EQ(
                vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device.physicalDevice(), surface.handle(), &capabilities),
                VK_SUCCESS);
            std::cout << "DEVICE=" << properties.deviceName << " API=" << VK_API_VERSION_MAJOR(properties.apiVersion)
                      << '.' << VK_API_VERSION_MINOR(properties.apiVersion) << '.'
                      << VK_API_VERSION_PATCH(properties.apiVersion) << " DRIVER=" << properties.driverVersion
                      << " SURFACE_MIN=" << capabilities.minImageCount << " MAX=" << capabilities.maxImageCount << '\n';
        }
        expectCleanValidation();
    }

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
            EXPECT_EQ(countOccurrences(messages_.str(), "Uploaded RGBA8 texture"), 1u);
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
                EXPECT_GT(image.rgba[offset + channel], 220);  // Direction texture starts with white.
            WindowCapture::save(image, "review/resource-entry-independent-textures.ppm");
            EXPECT_EQ(countOccurrences(messages_.str(), "Uploaded RGBA8 texture"), 2u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline"), 1u);
            EXPECT_EQ(firstAudit.textureOpens, 1u);
            EXPECT_EQ(secondAudit.textureOpens, 1u);
            // No explicit wait: closing protects the final submission and releases both textures.
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
            // A third material with another texture does not replace shaders, camera sets or Pipeline.
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
            // The reference's white texel shows the copied nonzero RGB tint after desc was cleared.
            const int x = bounds[0] + 7 * (bounds[2] - bounds[0]) / 8;
            const int y = bounds[1] + (bounds[3] - bounds[1]) / 2;
            const auto offset = (static_cast<size_t>(y) * image.width + x) * 4;
            EXPECT_GT(image.rgba[offset], 50);
            EXPECT_GT(int(image.rgba[offset + 1]), int(image.rgba[offset]) + 20);
            EXPECT_GT(int(image.rgba[offset + 2]), int(image.rgba[offset + 1]) + 20);
            WindowCapture::save(image, "review/material-immutable-tint.ppm");
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline"), 1u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Uploaded RGBA8 texture"), 2u);
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
                // The first item is valid: a later failure must reject the entire list before recording.
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
            for (const float tint : {1.0f, 0.5f})
            {
                SceneAssets assets(referenceSceneAssetPaths());
                const auto pixels = TextureGpuReview::capture(instance.handle(), device, *window, assets,
                                                              swapchain.format(), {tint, tint, tint});
                const size_t white = (112 * 384 + 240) * 4;
                const size_t gray = (112 * 384 + 176) * 4;
                for (int channel = 0; channel < 3; ++channel)
                {
                    EXPECT_NEAR(pixels[white + channel], tint == 1.0f ? 255 : 188, 1);
                    EXPECT_NEAR(pixels[gray + channel], tint == 1.0f ? 128 : 92, 1);
                    EXPECT_NEAR(pixels[(64 * 384 + 320) * 4 + channel], 128, 1);   // UI is not tinted.
                    EXPECT_NEAR(pixels[(160 * 384 + 320) * 4 + channel], 128, 1);  // Clear is not tinted.
                }
                for (int y = 0; y < 3; ++y)
                    for (int x = 0; x < 4; ++x)
                        EXPECT_EQ(pixels[((16 + y * 64 + 32) * 384 + 16 + x * 64 + 32) * 4 + 3], 255);
                if (tint == 0.5f)
                    WindowCapture::save({384, 256, pixels}, "review/material-half-linear-gpu.ppm");
                expectReleased(referenceSceneAssetPaths());
                std::cout << "GPU_MATERIAL: FORMAT=" << int(swapchain.format())
                          << " COLOR_SPACE=" << int(swapchain.colorSpace()) << " LINEAR_TINT=" << tint
                          << " WHITE=" << int(pixels[white]) << " GRAY=" << int(pixels[gray])
                          << " ALPHA=" << int(pixels[white + 3]) << " RGB_TOLERANCE=1\n";
            }
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
            EXPECT_EQ(countOccurrences(messages_.str(), "Uploaded RGBA8 texture:"), 1u);
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
                    // Display checks have a wider tolerance than the separate offscreen numeric test.
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
            EXPECT_EQ(countOccurrences(messages_.str(), "Uploaded RGBA8 texture"), 2u);
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

    TEST_F(VulkanReviewTest, RequiredValidationDrawResizeAndShutdown)
    {
        auto window = createWindow();
        for (const auto texture : {"textures/reference_rgba.png", "textures/direction_rgba.png"})
        {
            FileAudit audit;
            audit.paths.texture = texture;
            {
                SceneAssets assets(audit.paths, std::make_unique<AuditedFileIO>(audit));
                Renderer renderer(*window, RendererValidationMode::Required);
                renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
                const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
                const auto mesh = createCubeMesh(renderer);
                releaseCpuAssets(assets);
                for (int cycle = 0; cycle < 4; ++cycle)
                {
                    glfwSetWindowSize(static_cast<GLFWwindow*>(window->getNativeHandle()), 680 + cycle * 30,
                                      400 + cycle * 20);
                    int recorded = 0;
                    for (int frame = 0; frame < 5; ++frame)
                        recorded += drawUiFrame(*window, renderer, mesh, material);
                    EXPECT_GT(recorded, 0);
                    EXPECT_EQ(audit.textureOpens, 1u);
                }
            }
            EXPECT_TRUE(audit.destroyed);
            EXPECT_TRUE(audit.routesClearedBeforeDestroy);
            EXPECT_TRUE(audit.cpuReleasedBeforeDestroy);
            expectReleased(audit.paths);
            std::cout << "TEXTURE=" << texture << " RESIZE_CYCLES=4 PNG_OPENS=" << audit.textureOpens
                      << " CPU_RELEASED_BEFORE_FILEIO=" << audit.cpuReleasedBeforeDestroy << '\n';
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, PartialAssetConstructionReleasesBeforeFixtureCleanup)
    {
        // A valid header with truncated IDAT reaches the decoder and deterministically fails.
        const auto truncatedPath = std::filesystem::path("assets/textures/task05-truncated.png");
        {
            std::ifstream input("assets/textures/reference_rgba.png", std::ios::binary);
            std::array<char, 72> bytes{};
            ASSERT_TRUE(input.read(bytes.data(), bytes.size()));
            std::ofstream output(truncatedPath, std::ios::binary);
            output.write(bytes.data(), bytes.size());
            ASSERT_TRUE(output.good());
        }
        for (int stage = 0; stage < 4; ++stage)
        {
            Demo::SceneAssetPaths paths;
            if (stage == 0)
                paths.vertex = "shaders/missing-first.spv";
            if (stage == 1)
                paths.fragment = "shaders/missing-second.spv";
            if (stage == 2)
                paths.texture = "textures/missing-third.png";
            if (stage == 3)
                paths.texture = "textures/task05-truncated.png";
            FileAudit audit;
            audit.paths = paths;
            EXPECT_THROW((SceneAssets(paths, std::make_unique<AuditedFileIO>(audit))), Error::Exception);
            EXPECT_TRUE(audit.destroyed);
            EXPECT_TRUE(audit.routesClearedBeforeDestroy);
            EXPECT_TRUE(audit.cpuReleasedBeforeDestroy);
            expectReleased(paths);
        }
        EXPECT_TRUE(std::filesystem::remove(truncatedPath));
    }

    TEST_F(VulkanReviewTest, AlternateDemoTextureMatchesCpuAndGpuAndReleasesBeforeFileIO)
    {
        auto window = createWindow(false, 384, 256);
        Demo::SceneAssetPaths paths;
        paths.texture = "textures/direction_rgba.png";
        FileAudit audit;
        audit.paths = paths;
        {
            SceneAssets assets(paths, std::make_unique<AuditedFileIO>(audit));
            ASSERT_EQ(assets.texture->width(), 3u);
            ASSERT_EQ(assets.texture->height(), 4u);
            const std::vector<uint8_t> expected = {
                255, 255, 255, 255, 255, 128, 0,  255, 0,   0,   0, 255, 0,   0, 255, 255,
                128, 128, 128, 255, 255, 0,   0,  255, 0,   255, 0, 255, 255, 0, 255, 128,
                0,   255, 255, 255, 17,  34,  51, 0,   255, 255, 0, 255, 128, 0, 255, 255,
            };
            EXPECT_EQ(assets.texture->pixels(), expected);
            VulkanInstance instance(requiredExtensions(), ValidationMode::Required);
            GlfwSurface surface(instance, *window);
            VulkanDevice device(instance, surface);
            VulkanSwapchain swapchain(device, surface, 384, 256);
            const auto pixels =
                TextureGpuReview::capture(instance.handle(), device, *window, assets, swapchain.format());
            for (int y = 0; y < 4; ++y)
            {
                for (int x = 0; x < 3; ++x)
                {
                    const size_t offset = ((16 + y * 48 + 24) * 384 + 16 + (2 * x + 1) * 256 / 6) * 4;
                    for (int channel = 0; channel < 3; ++channel)
                        EXPECT_NEAR(pixels[offset + channel], expected[(y * 3 + x) * 4 + channel], 1);
                    EXPECT_EQ(pixels[offset + 3], 255);
                }
            }
            expectReleased(paths);
            EXPECT_EQ(audit.textureOpens, 1u);
            std::filesystem::create_directories("review");
            std::ofstream output("review/texture-direction.ppm", std::ios::binary);
            output << "P6\n384 256\n255\n";
            for (size_t pixel = 0; pixel < pixels.size(); pixel += 4)
                output.write(reinterpret_cast<const char*>(pixels.data() + pixel), 3);
            ASSERT_TRUE(output.good());
            std::cout << "GPU_DIRECTION_FORMAT=" << int(swapchain.format())
                      << " COLOR_SPACE=" << int(swapchain.colorSpace()) << " RGB_TOLERANCE=1\n";
        }
        EXPECT_TRUE(audit.destroyed);
        EXPECT_TRUE(audit.routesClearedBeforeDestroy);
        EXPECT_TRUE(audit.cpuReleasedBeforeDestroy);
        // Restart with the other asset through the same explicit preparation as Demo.
        {
            SceneAssets assets(paths);
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            const auto mesh = createCubeMesh(renderer);
            releaseCpuAssets(assets);
            EXPECT_GT(drawUiFrame(*window, renderer, mesh, material), 0);
        }
        expectReleased(paths);
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, RendererInitializationFailureReleasesBeforeFixtureCleanup)
    {
        auto native = createWindow();
        DeferredFramebufferWindow window(*native);
        window.throwOnSize = true;
        FileAudit audit;
        EXPECT_THROW(([&] {
                         SceneAssets assets({}, std::make_unique<AuditedFileIO>(audit));
                         Renderer renderer(window, RendererValidationMode::Required);
                     }()),
                     Error::Exception);
        EXPECT_TRUE(audit.destroyed);
        EXPECT_TRUE(audit.routesClearedBeforeDestroy);
        EXPECT_TRUE(audit.cpuReleasedBeforeDestroy);
        EXPECT_EQ(audit.textureOpens, 1u);
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, AssetScopePreservesExistingOwnersAndReacquiresZeroReferenceEntries)
    {
        auto& registry = Resource::ResourceRegistry::instance();
        auto& manager = Resource::ResourceManager::instance();
        const auto held = manager.registerResource(Resource::ResourceType::TEXTURE, "textures/reference_rgba.png");
        {
            SceneAssets assets(referenceSceneAssetPaths());
            EXPECT_EQ(registry.refCount(held), 2u);
        }
        EXPECT_EQ(registry.refCount(held), 1u);
        EXPECT_NE(registry.getObject(held), nullptr);
        manager.release(held);
        EXPECT_EQ(registry.getObject(held), nullptr);
        {
            SceneAssets assets(referenceSceneAssetPaths());
            EXPECT_EQ(registry.refCount(held), 1u);
            releaseCpuAssets(assets);
        }
        expectReleased(referenceSceneAssetPaths());
    }

    TEST_F(VulkanReviewTest, ActualShaderAndPlaneMatchReferenceAndUiGrayOnGpu)
    {
        auto window = createWindow(false, 384, 256);
        {
            SceneAssets assets(referenceSceneAssetPaths());
            VulkanInstance instance(requiredExtensions(), ValidationMode::Required);
            GlfwSurface surface(instance, *window);
            VulkanDevice device(instance, surface);
            VulkanSwapchain swapchain(device, surface, 384, 256);
            EXPECT_EQ(swapchain.colorSpace(), VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
            const auto pixels =
                TextureGpuReview::capture(instance.handle(), device, *window, assets, swapchain.format());
            const std::array<std::array<int, 3>, 12> expected = {{{255, 0, 0},
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
            for (int y = 0; y < 3; ++y)
            {
                for (int x = 0; x < 4; ++x)
                {
                    const size_t offset = ((16 + y * 64 + 32) * 384 + 16 + x * 64 + 32) * 4;
                    for (int channel = 0; channel < 3; ++channel)
                        EXPECT_NEAR(pixels[offset + channel], expected[y * 4 + x][channel], 1);
                    EXPECT_EQ(pixels[offset + 3], 255);
                }
            }
            for (const auto position : {std::array{320, 64}, std::array{320, 160}})
            {
                const auto offset = (position[1] * 384 + position[0]) * 4;
                for (int channel = 0; channel < 3; ++channel)
                    EXPECT_NEAR(pixels[offset + channel], 128, 1);
            }
            const auto marker = (34 * 384 + 290) * 4;
            EXPECT_EQ(pixels[marker], 255);
            EXPECT_EQ(pixels[marker + 1], 0);
            EXPECT_EQ(pixels[marker + 2], 255);
            const auto blended = (200 * 384 + 320) * 4;
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(pixels[blended + channel], 64, 1);
            expectReleased(referenceSceneAssetPaths());
            std::filesystem::create_directories("review");
            std::ofstream output("review/task04-color.ppm", std::ios::binary);
            output << "P6\n384 256\n255\n";
            for (size_t pixel = 0; pixel < pixels.size(); pixel += 4)
                output.write(reinterpret_cast<const char*>(pixels.data() + pixel), 3);
            ASSERT_TRUE(output.good());
            std::cout << "GPU_COLOR_FORMAT=" << int(swapchain.format())
                      << " COLOR_SPACE=" << int(swapchain.colorSpace())
                      << " CPU_UNLOADED_BEFORE_DRAW=1 GRAY_TEXEL_UI_CLEAR=128(+/-1)\n";
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
            // Copies share one upload after the CPU arrays and original reference are gone.
            for (int frameIndex = 0; frameIndex < 6; ++frameIndex)
                EXPECT_GT(drawUiFrame(*window, renderer, observer, material), 0);
            Renderer moved(std::move(renderer));
            EXPECT_THROW(renderer.createMesh({}, {}), Error::Exception);
            EXPECT_GT(drawUiFrame(*window, moved, observer, material), 0);
            // No explicit wait: Renderer protects the last submission and frees meshes before Device.
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
                // CreateContext preserves an existing current context; this test needs a separate one.
                ImGui::SetCurrentContext(nullptr);
                Renderer second(*secondWindow, RendererValidationMode::Required);
                second.prepareSceneResources(*assets.vertex, *assets.fragment);
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
            for (int frame = 0; frame < 5; ++frame)
                EXPECT_FALSE(renderer.beginUiFrame());
            EXPECT_EQ(ImGui::GetFrameCount(), 0);
            window.drawable = true;
            int recorded = 0;
            for (int frame = 0; frame < 10; ++frame)
            {
                if (prepareScene)
                    recorded += drawUiFrame(window, renderer, mesh, material);
                else
                {
                    submitControlledScene(window, renderer, {}, true);
                    ++recorded;
                }
            }
            EXPECT_GT(recorded, 0);
            EXPECT_NE(ImGui::GetIO().BackendRendererUserData, nullptr);
            EXPECT_TRUE(ImGui::GetIO().Fonts->IsBuilt());
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
                glfwSetWindowSize(native, 680 + cycle * 20, 400 + cycle * 10);
                EXPECT_GT(drawUiFrame(*window, renderer, mesh, material), 0);
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
        // One, two, and three submitted frames exercise each distinct shutdown boundary.
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
            // No explicit wait here: the destructor must protect the last GPU submission.
        }
        expectCleanValidation();
    }
    TEST_F(VulkanReviewTest, SwapchainLogsOnlyCreationAndStateChanges)
    {
        auto native = createWindow();
        DeferredFramebufferWindow window(*native);
        SceneAssets assets;
        {
            Renderer renderer(window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            const auto mesh = createCubeMesh(renderer);
            releaseCpuAssets(assets);
            for (int frame = 0; frame < 10; ++frame)
                EXPECT_FALSE(renderer.beginUiFrame());
            EXPECT_EQ(countOccurrences(messages_.str(), "Swapchain "), 0u);
            window.drawable = true;
            for (int frame = 0; frame < 20; ++frame)
                drawUiFrame(window, renderer, mesh, material);
            EXPECT_EQ(countOccurrences(messages_.str(), "Swapchain initialization:"), 1u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Swapchain "), 1u);
            EXPECT_NE(messages_.str().find("640x360, format "), std::string::npos);
            EXPECT_NE(messages_.str().find("images, present mode FIFO"), std::string::npos);
            glfwSetWindowSize(static_cast<GLFWwindow*>(native->getNativeHandle()), 720, 440);
            for (int frame = 0; frame < 10; ++frame)
                drawUiFrame(window, renderer, mesh, material);
            EXPECT_EQ(countOccurrences(messages_.str(), "Swapchain framebuffer size changed:"), 1u);
            const auto beforeSkip = messages_.str();
            window.drawable = false;
            for (int frame = 0; frame < 10; ++frame)
                EXPECT_FALSE(renderer.beginUiFrame());
            EXPECT_EQ(messages_.str(), beforeSkip);
            window.drawable = true;
            EXPECT_GT(drawUiFrame(window, renderer, mesh, material), 0);
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
                // Exercise the public submit path across both GPU frame slots. After return,
                // changing application input must not change uniforms already submitted.
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
            // Compare scene pixels, excluding Windows' rounded corners and desktop border pixels.
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
            // The visible +Z top row must still read red, green, blue, yellow.
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
                // First four frames use different meshes; last two share the plane upload.
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
                // Wait for desktop presentation only; production has no added device-idle wait.
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

    TEST_F(VulkanReviewTest, DemoControlsDrivePresentedSceneWithBothTextures)
    {
        auto window = createWindow(true, 1280, 720);
        for (const auto texture : {"textures/reference_rgba.png", "textures/direction_rgba.png"})
        {
            FileAudit audit;
            audit.paths.texture = texture;
            {
                SceneAssets assets(audit.paths, std::make_unique<AuditedFileIO>(audit));
                Renderer renderer(*window, RendererValidationMode::Required);
                renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
                const auto gpuTexture = renderer.createTexture(*assets.texture);
                const auto material = renderer.createMaterial({gpuTexture});
                const auto planeMaterial = renderer.createMaterial({gpuTexture, {0.5f, 1.0f, 0.5f}});
                const auto cube = createCubeMesh(renderer);
                const auto plane = [&] {
                    const auto data = Demo::makePlaneMesh();
                    return renderer.createMesh(data.vertices, data.indices);
                }();
                releaseCpuAssets(assets);
                Demo::SceneControls scene;
                const auto frame = [&] {
                    window->pollEvents();
                    const auto extent = renderer.beginUiFrame();
                    if (!extent)
                        throw Error::Exception(Error::Code::RENDER, "Demo review requires a drawable frame");
                    Demo::drawSceneControls(scene, texture);
                    const auto draws = Demo::makeSceneDraws(scene, cube, plane, material, planeMaterial);
                    const auto input = Demo::makeSceneFrame(*extent, draws, scene.camera);
                    renderer.imgui().endFrame();
                    auto* ui = renderer.imgui().drawData();
                    renderer.drawFrame(input, scene.clearColor,
                                       [&](void* commands) { renderer.imgui().recordDrawData(ui, commands); });
                    return draws.size();
                };
                auto& controlledIo = ImGui::GetIO();
                // Warmup must not retain desktop input before the synthetic stream takes over.
                controlledIo.ClearEventsQueue();
                controlledIo.ClearInputKeys();
                controlledIo.ClearInputMouse();
                controlledIo.ConfigInputTrickleEventQueue = false;
                controlledIo.SetAppAcceptingEvents(false);
                for (int warmup = 0; warmup < 3; ++warmup)
                    EXPECT_EQ(frame(), 2u);
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                const bool reference = audit.paths.texture == "textures/reference_rgba.png";
                WindowCapture::save(WindowCapture::capture(*window),
                                    reference ? "review/demo-multi-reference.ppm" : "review/demo-multi-direction.ppm");
                const auto click = [&](float x, float y, float dragX = 0.0f) {
                    auto& io = ImGui::GetIO();
                    // Isolate this controlled input stream from desktop cursor/focus events.
                    io.SetAppAcceptingEvents(true);
                    io.AddFocusEvent(true);
                    io.AddMousePosEvent(x, y);
                    io.SetAppAcceptingEvents(false);
                    frame();
                    EXPECT_FLOAT_EQ(io.MousePos.x, x);
                    EXPECT_FLOAT_EQ(io.MousePos.y, y);
                    io.SetAppAcceptingEvents(true);
                    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                    io.SetAppAcceptingEvents(false);
                    frame();
                    EXPECT_TRUE(io.MouseDown[ImGuiMouseButton_Left]);
                    if (dragX != 0.0f)
                    {
                        io.SetAppAcceptingEvents(true);
                        io.AddMousePosEvent(x + dragX, y);
                        io.SetAppAcceptingEvents(false);
                        frame();
                    }
                    io.SetAppAcceptingEvents(true);
                    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                    io.SetAppAcceptingEvents(false);
                    return frame();
                };
                // Client coordinates of the Demo's default 350-pixel panel and default font.
                // Exercise the actual widgets through ImGui input, without scene-state test hooks.
                const auto initial = Demo::makeSceneDraws(scene, cube, plane, material, planeMaterial);
                click(60, 135);
                click(61, 158, 10);
                click(58, 181);
                EXPECT_NE(scene.cube.transform.position.x, -0.8f);
                EXPECT_NE(scene.cube.transform.rotationDegrees.x, 20.0f);
                EXPECT_NE(scene.cube.transform.scale.x, 1.0f);
                auto edited = Demo::makeSceneDraws(scene, cube, plane, material, planeMaterial);
                EXPECT_NE(edited[0].model, initial[0].model);
                EXPECT_EQ(edited[1].model, initial[1].model);
                const auto cubeModel = edited[0].model;
                click(60, 250, -12);
                click(61, 273, 8);
                click(58, 296, 8);
                EXPECT_NE(scene.plane.transform.position.x, 0.8f);
                EXPECT_NE(scene.plane.transform.rotationDegrees.x, 0.0f);
                EXPECT_NE(scene.plane.transform.scale.x, 1.3f);
                edited = Demo::makeSceneDraws(scene, cube, plane, material, planeMaterial);
                EXPECT_EQ(edited[0].model, cubeModel);
                EXPECT_NE(edited[1].model, initial[1].model);
                const auto beforeCamera = Demo::makeSceneFrame({1280, 720}, edited, scene.camera);
                click(52, 342, 8);
                click(147, 365);
                click(144, 388);
                click(124, 411);
                click(145, 434);
                EXPECT_NE(scene.camera.target.x, 0.0f);
                EXPECT_NE(scene.camera.yawDegrees, 0.0f);
                EXPECT_NE(scene.camera.pitchDegrees, 15.0f);
                EXPECT_NE(scene.camera.distance, 5.0f);
                EXPECT_NE(scene.camera.fieldOfViewDegrees, 45.0f);
                const auto afterCamera = Demo::makeSceneFrame({1280, 720}, edited, scene.camera);
                EXPECT_NE(afterCamera.camera.view, beforeCamera.camera.view);
                EXPECT_NE(afterCamera.camera.projection, beforeCamera.camera.projection);
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                if (reference)
                    WindowCapture::save(WindowCapture::capture(*window), "review/demo-multi-edited.ppm");
                EXPECT_EQ(click(30, 111), 1u);
                EXPECT_FALSE(scene.cube.visible);
                EXPECT_EQ(click(30, 227), 0u);
                EXPECT_FALSE(scene.plane.visible);
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                const auto empty = WindowCapture::capture(*window);
                if (reference)
                    WindowCapture::save(empty, "review/demo-multi-empty.ppm");
                size_t scenePixels = 0;
                for (int y = 8; y < empty.height - 8; ++y)
                    for (int x = 380; x < empty.width - 8; ++x)
                    {
                        const size_t pixel = (static_cast<size_t>(y) * empty.width + x) * 4;
                        int difference = 0;
                        for (int channel = 0; channel < 3; ++channel)
                            difference += std::abs(int(empty.rgba[pixel + channel]) - int(empty.rgba[channel]));
                        scenePixels += difference > 12;
                    }
                EXPECT_EQ(scenePixels, 0u);
                EXPECT_EQ(click(30, 111), 1u);
                EXPECT_EQ(click(30, 227), 2u);
                EXPECT_EQ(click(60, 457), 2u);
                const Demo::SceneControls defaults;
                EXPECT_EQ(scene.cube.transform.position, defaults.cube.transform.position);
                EXPECT_EQ(scene.plane.transform.scale, defaults.plane.transform.scale);
                EXPECT_EQ(scene.camera.yawDegrees, defaults.camera.yawDegrees);
                ImGui::GetIO().SetAppAcceptingEvents(true);
                EXPECT_EQ(audit.textureOpens, 1u);
                std::cout << "DEMO_UI: TEXTURE=" << texture
                          << " INDEPENDENT_OBJECT_EDITS=2 CAMERA_CONTROLS=5 VISIBILITY=2,1,0,1,2 RESET=1 PNG_OPENS="
                          << audit.textureOpens << '\n';
            }
            expectReleased(audit.paths);
        }
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

    TEST_F(VulkanReviewTest, DefaultDemoPolicyMatchesBuild)
    {
        auto window = createWindow();
        SceneAssets assets;
#ifdef _DEBUG
        EXPECT_EQ(Demo::VALIDATION_MODE, RendererValidationMode::Optional);
#else
        EXPECT_EQ(Demo::VALIDATION_MODE, RendererValidationMode::Disabled);
#endif
        {
            Renderer renderer(*window, Demo::VALIDATION_MODE);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            const auto mesh = createCubeMesh(renderer);
            releaseCpuAssets(assets);
            for (int frame = 0; frame < 5; ++frame)
                EXPECT_GT(drawUiFrame(*window, renderer, mesh, material), 0);
        }
#ifdef _DEBUG
        EXPECT_NE(messages_.str().find("validation: enabled"), std::string::npos);
#else
        EXPECT_NE(messages_.str().find("validation: disabled"), std::string::npos);
#endif
        EXPECT_EQ(messages_.str().find("warning|"), std::string::npos);
        EXPECT_EQ(messages_.str().find("error|"), std::string::npos);
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
