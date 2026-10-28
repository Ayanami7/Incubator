#include "runtime/render/integration/imgui_renderer.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <limits>
#include <span>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/platform/fileio/fileio.h"
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
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
                // 只使用主窗口，平台多窗口与布局持久化由后续工作区任务决定。
                io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
                try
                {
                    auto fileIO = FileIO::createPlatformFileIO();
                    auto file = fileIO->open("assets/fonts/NotoSansSC-Regular.otf", FileIO::OpenMode::Read);
                    if (!file || file->size() == 0 || file->size() > std::numeric_limits<int>::max())
                        throw Error::Exception(Error::Code::IO, "Cannot load UI font assets/fonts/NotoSansSC-Regular.otf");
                    fontData_.resize(static_cast<size_t>(file->size()));
                    size_t offset = 0;
                    while (offset < fontData_.size())
                    {
                        const auto count = file->read(std::span(fontData_).subspan(offset));
                        if (!count)
                            throw Error::Exception(Error::Code::IO, "Incomplete UI font read");
                        offset += count;
                    }
                    updateDpi();
                    auto* glfwWindow = static_cast<GLFWwindow*>(window_.getNativeHandle());
                    if (!glfwWindow || !ImGui_ImplGlfw_InitForVulkan(glfwWindow, true))
                        throw Error::Exception(Error::Code::RENDER, "Failed to initialize ImGui GLFW backend");
                    glfwInitialized_ = true;
                }
                catch (...)
                {
                    ImGui::DestroyContext();
                    contextCreated_ = false;
                    throw;
                }
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
                Log::Registry::get("render").debug(
                    "Initialized ImGui Vulkan backend: {} images, color format {}, depth format {}, queue family {}",
                    imageCount_, static_cast<int>(colorFormat_), static_cast<int>(depthFormat_), queueFamily_);
            }

            void ImGuiRenderer::beginFrame()
            {
                ImGui_ImplGlfw_NewFrame();
                updateDpi();
                ImGui_ImplVulkan_NewFrame();
                ImGui::NewFrame();
            }

            void ImGuiRenderer::updateDpi()
            {
                const float contentScale = window_.getContentScale();
                int width, height, framebufferWidth, framebufferHeight;
                window_.getWindowSize(width, height);
                window_.getFramebufferSize(framebufferWidth, framebufferHeight);
                const float framebufferScale = height > 0 && framebufferHeight > 0
                                                   ? float(framebufferHeight) / float(height)
                                                   : 1.0f;
                if (contentScale == contentScale_ && framebufferScale == framebufferScale_)
                    return;

                // 在 NewFrame 之前等待旧字体的所有 GPU 使用结束，不使本轮绘制数据失效。
                if (vulkanInitialized_ && vkDeviceWaitIdle(device_) != VK_SUCCESS)
                    throw Error::Exception(Error::Code::RENDER, "Failed to wait before UI font rebuild");
                auto& io = ImGui::GetIO();
                io.Fonts->Clear();
                ImFontConfig fontConfig;
                fontConfig.FontDataOwnedByAtlas = false;
                io.FontDefault = io.Fonts->AddFontFromMemoryTTF(
                    fontData_.data(), static_cast<int>(fontData_.size()), 18.0f * contentScale, &fontConfig,
                    io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                if (!io.FontDefault || !io.Fonts->Build())
                    throw Error::Exception(Error::Code::RENDER, "Failed to build UI font atlas");
                io.FontGlobalScale = 1.0f / framebufferScale;

                // 每次从相同的基础样式计算，避免 DPI 往返时累积缩放。
                ImGuiStyle style;
                ImGui::StyleColorsDark(&style);
                style.WindowPadding = {12, 12};
                style.FramePadding = {8, 6};
                style.ItemSpacing = {8, 8};
                style.ItemInnerSpacing = {6, 4};
                style.ScrollbarSize = 16;
                style.WindowRounding = 4;
                style.FrameRounding = 3;
                style.GrabRounding = 3;
                style.ScaleAllSizes(contentScale / framebufferScale);
                ImGui::GetStyle() = style;
                if (vulkanInitialized_ && !ImGui_ImplVulkan_CreateFontsTexture())
                    throw Error::Exception(Error::Code::RENDER, "Failed to upload rebuilt UI font texture");
                contentScale_ = contentScale;
                framebufferScale_ = framebufferScale;
                Log::Registry::get("render").info("UI font rebuilt: DPI {}, framebuffer scale {}, atlas {}x{}",
                                                 contentScale, framebufferScale, io.Fonts->TexWidth,
                                                 io.Fonts->TexHeight);
            }

            uint64_t ImGuiRenderer::sceneImage(uint32_t slot, VkImageView image)
            {
                if (!vulkanInitialized_ || slot >= sceneBindings_.size() || !image)
                    throw Error::Exception(Error::Code::RENDER, "Invalid UI scene image");
                if (!sceneSampler_)
                {
                    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                    info.magFilter = info.minFilter = VK_FILTER_LINEAR;
                    info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                    if (vkCreateSampler(device_, &info, nullptr, &sceneSampler_) != VK_SUCCESS)
                        throw Error::Exception(Error::Code::RENDER, "Cannot create scene image sampler");
                }
                auto& binding = sceneBindings_[slot];
                if (binding.image != image)
                {
                    releaseSceneImage(slot);
                    binding.set =
                        ImGui_ImplVulkan_AddTexture(sceneSampler_, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    binding.image = image;
                }
                return reinterpret_cast<uint64_t>(binding.set);
            }
            void ImGuiRenderer::releaseSceneImage(uint32_t slot)
            {
                auto& binding = sceneBindings_.at(slot);
                if (binding.set)
                    ImGui_ImplVulkan_RemoveTexture(binding.set);
                binding = {};
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
                    for (uint32_t slot = 0; slot < sceneBindings_.size(); ++slot)
                        releaseSceneImage(slot);
                    if (sceneSampler_)
                        vkDestroySampler(device_, sceneSampler_, nullptr);
                    sceneSampler_ = VK_NULL_HANDLE;
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
