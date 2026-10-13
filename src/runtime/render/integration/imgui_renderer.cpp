#include "runtime/render/integration/imgui_renderer.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/platform/window/window.h"

namespace Incubator
{
    namespace Render
    {
        namespace Integration
        {
            ImGuiRenderer::ImGuiRenderer(Window::Window& window) : window_(window)
            {
                IMGUI_CHECKVERSION();
                ImGui::CreateContext();
                contextCreated_ = true;
                ImGuiIO& io = ImGui::GetIO();
                io.IniFilename = nullptr;
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
                ImGui::StyleColorsDark();

                auto* glfwWindow = static_cast<GLFWwindow*>(window_.getNativeHandle());
                if (!glfwWindow || !ImGui_ImplGlfw_InitForVulkan(glfwWindow, true))
                {
                    ImGui::DestroyContext();
                    contextCreated_ = false;
                    throw Error::Exception(Error::Code::RENDER, "Failed to initialize ImGui GLFW backend");
                }
                glfwInitialized_ = true;
            }

            ImGuiRenderer::~ImGuiRenderer()
            {
                shutdownVulkan();
                if (glfwInitialized_)
                    ImGui_ImplGlfw_Shutdown();
                if (contextCreated_)
                    ImGui::DestroyContext();
            }

            void ImGuiRenderer::initializeVulkan(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
                                                 uint32_t queueFamily, VkQueue queue, VkFormat colorFormat,
                                                 VkFormat depthFormat, uint32_t imageCount)
            {
                if (imageCount < 2)
                    throw Error::Exception(Error::Code::RENDER,
                                           fmt::format("The ImGui Vulkan backend requires at least two swapchain "
                                                       "images; received {}", imageCount));
                if (vulkanInitialized_ && instance_ == instance && physicalDevice_ == physicalDevice &&
                    device_ == device && colorFormat_ == colorFormat && depthFormat_ == depthFormat &&
                    imageCount_ == imageCount)
                    return;
                shutdownVulkan();

                instance_ = instance;
                physicalDevice_ = physicalDevice;
                device_ = device;
                queueFamily_ = queueFamily;
                queue_ = queue;
                colorFormat_ = colorFormat;
                depthFormat_ = depthFormat;
                imageCount_ = imageCount;

                ImGui_ImplVulkan_InitInfo info{};
                info.ApiVersion = VK_API_VERSION_1_3;
                info.Instance = instance_;
                info.PhysicalDevice = physicalDevice_;
                info.Device = device_;
                info.QueueFamily = queueFamily_;
                info.Queue = queue_;
                info.DescriptorPoolSize = 128;
                info.MinImageCount = 2;
                info.ImageCount = imageCount_;
                info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
                info.UseDynamicRendering = true;
                info.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
                info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
                info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat_;
                info.PipelineRenderingCreateInfo.depthAttachmentFormat = depthFormat_;
                if (!ImGui_ImplVulkan_Init(&info))
                    throw Error::Exception(Error::Code::RENDER, "Failed to initialize ImGui Vulkan backend");
                vulkanInitialized_ = true;
                if (!ImGui_ImplVulkan_CreateFontsTexture())
                {
                    shutdownVulkan();
                    throw Error::Exception(Error::Code::RENDER, "Failed to upload ImGui font texture");
                }
            }

            void ImGuiRenderer::beginFrame()
            {
                ImGui_ImplVulkan_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();
            }

            void ImGuiRenderer::endFrame()
            {
                ImGui::Render();
            }

            void* ImGuiRenderer::drawData() const noexcept
            {
                return ImGui::GetDrawData();
            }

            void ImGuiRenderer::recordDrawData(void* drawData, void* nativeCommandBuffer) const
            {
                if (drawData && nativeCommandBuffer && vulkanInitialized_)
                {
                    ImGui_ImplVulkan_RenderDrawData(static_cast<ImDrawData*>(drawData),
                                                    static_cast<VkCommandBuffer>(nativeCommandBuffer));
                }
            }

            void ImGuiRenderer::shutdownVulkan() noexcept
            {
                if (vulkanInitialized_)
                {
                    const VkResult idleResult = vkDeviceWaitIdle(device_);
                    if (idleResult != VK_SUCCESS)
                        Log::Registry::get("render").error("vkDeviceWaitIdle before ImGui shutdown failed: {}",
                                                             static_cast<int>(idleResult));
                    ImGui_ImplVulkan_Shutdown();
                }
                vulkanInitialized_ = false;
                instance_ = VK_NULL_HANDLE;
                physicalDevice_ = VK_NULL_HANDLE;
                device_ = VK_NULL_HANDLE;
                queueFamily_ = VK_QUEUE_FAMILY_IGNORED;
                queue_ = VK_NULL_HANDLE;
                colorFormat_ = VK_FORMAT_UNDEFINED;
                depthFormat_ = VK_FORMAT_UNDEFINED;
                imageCount_ = 0;
            }
        }  // namespace Integration
    }  // namespace Render
}  // namespace Incubator
