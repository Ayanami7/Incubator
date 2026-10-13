#include <GLFW/glfw3.h>
#include <gtest/gtest.h>
#include <imgui.h>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string_view>
#include <vector>

#include "examples/demo/validation_mode.h"
#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/platform/window/window.h"
#include "runtime/render/integration/imgui_renderer.h"
#include "runtime/render/renderer.h"
#include "runtime/render/vulkan/glfw_surface.h"
#include "runtime/render/vulkan/vulkan_device.h"
#include "runtime/render/vulkan/vulkan_instance.h"
#include "runtime/resource/loader/shader_loader.h"
#include "runtime/resource/loader/shader_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"

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

    class ShaderAssets
    {
    public:
        ShaderAssets() : fileIO_(FileIO::createPlatformFileIO())
        {
            auto& manager = Resource::ResourceManager::instance();
            manager.registerLoader(Resource::ResourceType::SHADER, std::make_unique<Resource::ShaderLoader>(*fileIO_));
            vertexId_ = manager.registerResource(Resource::ResourceType::SHADER, "shaders/cube.vert.spv");
            fragmentId_ = manager.registerResource(Resource::ResourceType::SHADER, "shaders/cube.frag.spv");
            vertex = manager.get<Resource::ShaderResource>(vertexId_);
            fragment = manager.get<Resource::ShaderResource>(fragmentId_);
            if (!vertex || !fragment)
                throw Error::Exception(Error::Code::RENDER, "Review shaders were not built");
        }

        ~ShaderAssets()
        {
            auto& manager = Resource::ResourceManager::instance();
            manager.release(vertexId_);
            manager.release(fragmentId_);
            manager.reset();
        }

        Resource::ShaderResource* vertex = nullptr;
        Resource::ShaderResource* fragment = nullptr;

    private:
        std::unique_ptr<FileIO::FileIO> fileIO_;
        Resource::ResourceId vertexId_;
        Resource::ResourceId fragmentId_;
    };

    // Simulates the application's initial unavailable framebuffer without mocking Vulkan.
    class DeferredFramebufferWindow final : public Window::Window
    {
    public:
        explicit DeferredFramebufferWindow(Incubator::Window::Window& native) : native_(native) {}
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

    private:
        Incubator::Window::Window& native_;
    };

    class VulkanReviewTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            Log::Registry::reset();
            Resource::ResourceManager::instance().reset();
            Resource::ResourceRegistry::instance().clear();
            auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(messages_);
            sink->set_pattern("%l|%v");
            Log::Registry::get("Render.Vulkan").addSink(sink);
            Log::Registry::get("render").addSink(sink);
        }

        void TearDown() override
        {
            Log::Registry::reset();
            Resource::ResourceManager::instance().reset();
            Resource::ResourceRegistry::instance().clear();
        }

        std::unique_ptr<Window::Window> createWindow(bool visible = false)
        {
            Window::WindowDesc desc;
            desc.title = "Incubator Vulkan review";
            desc.width = 640;
            desc.height = 360;
            desc.visible = visible;
            return Window::createPlatformWindow(desc);
        }

        int drawUiFrame(Window::Window& window, Renderer& renderer)
        {
            window.pollEvents();
            if (!renderer.beginUiFrame())
                return 0;
            ImGui::Begin("Vulkan review");
            ImGui::Text("Textured cube and UI lifecycle verification");
            ImGui::End();
            renderer.imgui().endFrame();
            auto* data = renderer.imgui().drawData();
            int recorded = 0;
            renderer.drawFrame({0.12f, 0.16f, 0.22f, 1.0f}, [&](void* commands) {
                ++recorded;
                renderer.imgui().recordDrawData(data, commands);
            });
            return recorded;
        }

        void expectCleanValidation()
        {
            const auto messages = messages_.str();
            std::cout << messages;
            EXPECT_NE(messages.find("validation: enabled"), std::string::npos);
            EXPECT_EQ(messages.find("warning|"), std::string::npos) << messages;
            EXPECT_EQ(messages.find("error|"), std::string::npos) << messages;
        }

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

    TEST_F(VulkanReviewTest, RequiredValidationDrawResizeAndShutdown)
    {
        auto window = createWindow();
        ShaderAssets shaders;
        {
            Renderer renderer(*window, *shaders.vertex, *shaders.fragment, RendererValidationMode::Required);
            for (int cycle = 0; cycle < 4; ++cycle)
            {
                glfwSetWindowSize(static_cast<GLFWwindow*>(window->getNativeHandle()), 640 + cycle * 30,
                                  360 + cycle * 20);
                int recorded = 0;
                for (int frame = 0; frame < 5; ++frame)
                    recorded += drawUiFrame(*window, renderer);
                EXPECT_GT(recorded, 0);
            }
        }
        expectCleanValidation();
    }
    TEST_F(VulkanReviewTest, FirstZeroFramebufferRestoresUiWithoutExtraResize)
    {
        auto native = createWindow();
        DeferredFramebufferWindow window(*native);
        ShaderAssets shaders;
        {
            Renderer renderer(window, *shaders.vertex, *shaders.fragment, RendererValidationMode::Required);
            EXPECT_EQ(ImGui::GetIO().BackendRendererUserData, nullptr);
            for (int frame = 0; frame < 5; ++frame)
                EXPECT_FALSE(renderer.beginUiFrame());
            EXPECT_EQ(ImGui::GetFrameCount(), 0);
            window.drawable = true;
            int recorded = 0;
            for (int frame = 0; frame < 10; ++frame)
                recorded += drawUiFrame(window, renderer);
            EXPECT_GT(recorded, 0);
            EXPECT_NE(ImGui::GetIO().BackendRendererUserData, nullptr);
            EXPECT_TRUE(ImGui::GetIO().Fonts->IsBuilt());
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, VisibleWindowMinimizeRestoreAndResize)
    {
        auto window = createWindow(true);
        ShaderAssets shaders;
        {
            Renderer renderer(*window, *shaders.vertex, *shaders.fragment, RendererValidationMode::Required);
            auto* native = static_cast<GLFWwindow*>(window->getNativeHandle());
            for (int cycle = 0; cycle < 4; ++cycle)
            {
                glfwSetWindowSize(native, 680 + cycle * 20, 400 + cycle * 10);
                EXPECT_GT(drawUiFrame(*window, renderer), 0);
                glfwIconifyWindow(native);
                for (int attempt = 0; attempt < 100 && !window->isIconified(); ++attempt)
                {
                    window->pollEvents();
                    glfwWaitEventsTimeout(0.005);
                }
                ASSERT_TRUE(window->isIconified());
                for (int frame = 0; frame < 5; ++frame)
                    EXPECT_EQ(drawUiFrame(*window, renderer), 0);
                glfwRestoreWindow(native);
                for (int attempt = 0; attempt < 100 && window->isIconified(); ++attempt)
                {
                    window->pollEvents();
                    glfwWaitEventsTimeout(0.005);
                }
                ASSERT_FALSE(window->isIconified());
                int recorded = 0;
                for (int frame = 0; frame < 10; ++frame)
                    recorded += drawUiFrame(*window, renderer);
                EXPECT_GT(recorded, 0);
            }
        }
        expectCleanValidation();
    }
    TEST_F(VulkanReviewTest, RapidUiShutdownAfterLastSubmission)
    {
        auto window = createWindow();
        ShaderAssets shaders;
        for (int cycle = 0; cycle < 12; ++cycle)
        {
            Renderer renderer(*window, *shaders.vertex, *shaders.fragment, RendererValidationMode::Required);
            int recorded = 0;
            for (int frame = 0; frame < 1 + cycle % 3; ++frame)
                recorded += drawUiFrame(*window, renderer);
            EXPECT_GT(recorded, 0);
            // No explicit wait here: the destructor must protect the last GPU submission.
        }
        expectCleanValidation();
    }
    TEST_F(VulkanReviewTest, SwapchainLogsOnlyCreationAndStateChanges)
    {
        auto native = createWindow();
        DeferredFramebufferWindow window(*native);
        ShaderAssets shaders;
        {
            Renderer renderer(window, *shaders.vertex, *shaders.fragment, RendererValidationMode::Required);
            for (int frame = 0; frame < 10; ++frame)
                EXPECT_FALSE(renderer.beginUiFrame());
            EXPECT_EQ(countOccurrences(messages_.str(), "Swapchain "), 0u);
            window.drawable = true;
            for (int frame = 0; frame < 20; ++frame)
                drawUiFrame(window, renderer);
            EXPECT_EQ(countOccurrences(messages_.str(), "Swapchain initialization:"), 1u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Swapchain "), 1u);
            EXPECT_NE(messages_.str().find("640x360, format "), std::string::npos);
            EXPECT_NE(messages_.str().find("images, present mode FIFO"), std::string::npos);
            glfwSetWindowSize(static_cast<GLFWwindow*>(native->getNativeHandle()), 720, 440);
            for (int frame = 0; frame < 10; ++frame)
                drawUiFrame(window, renderer);
            EXPECT_EQ(countOccurrences(messages_.str(), "Swapchain framebuffer size changed:"), 1u);
            const auto beforeSkip = messages_.str();
            window.drawable = false;
            for (int frame = 0; frame < 10; ++frame)
                EXPECT_FALSE(renderer.beginUiFrame());
            EXPECT_EQ(messages_.str(), beforeSkip);
            window.drawable = true;
            EXPECT_GT(drawUiFrame(window, renderer), 0);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanReviewTest, ResizeAfterAcquirePreservesFrameAndRebuilds)
    {
        auto window = createWindow();
        ShaderAssets shaders;
        {
            Renderer renderer(*window, *shaders.vertex, *shaders.fragment, RendererValidationMode::Required);
            ASSERT_TRUE(renderer.beginUiFrame());
            ImGui::Begin("Resize during acquired frame");
            ImGui::Text("Review");
            ImGui::End();
            renderer.imgui().endFrame();
            auto* data = renderer.imgui().drawData();
            renderer.drawFrame({0.1f, 0.2f, 0.3f, 1.0f}, [&](void* commands) {
                renderer.imgui().recordDrawData(data, commands);
                glfwSetWindowSize(static_cast<GLFWwindow*>(window->getNativeHandle()), 710, 430);
            });
            int recorded = 0;
            for (int frame = 0; frame < 10; ++frame)
                recorded += drawUiFrame(*window, renderer);
            EXPECT_GT(recorded, 0);
            EXPECT_NE(messages_.str().find("710x430"), std::string::npos);
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
        ShaderAssets shaders;
#ifdef _DEBUG
        EXPECT_EQ(Demo::VALIDATION_MODE, RendererValidationMode::Optional);
#else
        EXPECT_EQ(Demo::VALIDATION_MODE, RendererValidationMode::Disabled);
#endif
        {
            Renderer renderer(*window, *shaders.vertex, *shaders.fragment, Demo::VALIDATION_MODE);
            for (int frame = 0; frame < 5; ++frame)
                EXPECT_GT(drawUiFrame(*window, renderer), 0);
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
