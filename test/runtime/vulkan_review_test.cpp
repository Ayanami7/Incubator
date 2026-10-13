#include <GLFW/glfw3.h>
#include <gtest/gtest.h>
#include <imgui.h>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <sstream>
#include <string_view>
#include <vector>

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
            renderer.imgui().beginFrame();
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
}  // namespace
