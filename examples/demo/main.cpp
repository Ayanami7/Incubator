// Dear ImGui: standalone example application for Glfw + Vulkan

// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp

// Important note to the reader who wish to integrate imgui_impl_vulkan.cpp/.h in their own engine/app.
// - Common ImGui_ImplVulkan_XXX functions and structures are used to interface with imgui_impl_vulkan.cpp/.h.
//   You will use those if you want to use this rendering backend in your engine/app.
// - Helper ImGui_ImplVulkanH_XXX functions and structures are only used by this example (main.cpp) and by
//   the backend itself (imgui_impl_vulkan.cpp), but should PROBABLY NOT be used by your own engine/app code.
// Read comments in imgui_impl_vulkan.h.

#include <stdio.h>   // printf, fprintf
#include <stdlib.h>  // abort
#include <vector>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

// Incubator engine modules
#include "runtime/core/log/logger.h"
#include "runtime/core/error/error.h"
#include "runtime/core/clock/clock.h"
#include "runtime/platform/window/window.h"
#include "runtime/render/vulkan/glfw_surface.h"
#include "runtime/render/vulkan/vulkan_device.h"
#include "runtime/render/vulkan/vulkan_instance.h"

// Volk headers
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
#define VOLK_IMPLEMENTATION
#include <volk.h>
#endif

// [Win32] Our example includes a copy of glfw3.lib pre-compiled with VS2010 to maximize ease of testing and
// compatibility with old VS compilers. To link with VS2010-era libraries, VS2015+ requires linking with
// legacy_stdio_definitions.lib, which we do using this pragma. Your own project should not be affected, as you are
// likely to link with a newer binary of GLFW that is adequate for your version of Visual Studio.
#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif

// Data
static ImGui_ImplVulkanH_Window g_MainWindowData;
static std::vector<VkSemaphore> g_PresentSemaphores;
static uint32_t g_MinImageCount = 2;
static bool g_SwapChainRebuild = false;

static void check_vk_result(VkResult err)
{
    if (err == VK_SUCCESS)
        return;
    throw Incubator::Error::Exception(
        Incubator::Error::Code::RENDER,
        fmt::format("VkResult = {}", (int)err)
    );
}

static VkDescriptorPool createImGuiDescriptorPool(VkDevice device)
{
    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                     IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = poolSize.descriptorCount;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    check_vk_result(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool));
    return descriptorPool;
}

static std::vector<VkSemaphore> createPresentSemaphores(VkDevice device, uint32_t imageCount)
{
    VkSemaphoreCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    std::vector<VkSemaphore> semaphores(imageCount, VK_NULL_HANDLE);
    for (uint32_t index = 0; index < imageCount; ++index)
    {
        const VkResult result = vkCreateSemaphore(device, &createInfo, nullptr, &semaphores[index]);
        if (result != VK_SUCCESS)
        {
            for (uint32_t created = 0; created < index; ++created)
                vkDestroySemaphore(device, semaphores[created], nullptr);
            check_vk_result(result);
        }
    }
    return semaphores;
}

static void destroyPresentSemaphores(VkDevice device, std::vector<VkSemaphore>& semaphores)
{
    for (VkSemaphore semaphore : semaphores)
        vkDestroySemaphore(device, semaphore, nullptr);
    semaphores.clear();
}
// All the ImGui_ImplVulkanH_XXX structures/functions are optional helpers used by the demo.
// Your real engine/app may not use them.
static void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd,
                              const Incubator::Render::Vulkan::VulkanInstance& instance,
                              const Incubator::Render::Vulkan::GlfwSurface& surface,
                              const Incubator::Render::Vulkan::VulkanDevice& device, int width, int height)
{
    // The current ImGui helper requires one queue family for graphics and presentation.
    if (device.queueFamilies().graphics != device.queueFamilies().present)
    {
        throw Incubator::Error::Exception(
            Incubator::Error::Code::RENDER,
            "The current ImGui swapchain helper requires a shared graphics and present queue family");
    }

    wd->Surface = surface.handle();
    const VkFormat requestSurfaceImageFormat[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                                  VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM};
    const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        device.physicalDevice(), wd->Surface, requestSurfaceImageFormat,
        static_cast<size_t>(IM_ARRAYSIZE(requestSurfaceImageFormat)), requestSurfaceColorSpace);
    const VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(device.physicalDevice(), wd->Surface, &presentMode, 1);

    IM_ASSERT(g_MinImageCount >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(instance.handle(), device.physicalDevice(), device.handle(), wd,
                                           device.queueFamilies().graphics, nullptr, width, height, g_MinImageCount);
}

static void CleanupVulkanWindow(const Incubator::Render::Vulkan::VulkanDevice& device)
{
    check_vk_result(vkDeviceWaitIdle(device.handle()));
    for (int index = 0; index < g_MainWindowData.Frames.Size; ++index)
    {
        ImGui_ImplVulkanH_Frame& frame = g_MainWindowData.Frames[index];
        vkDestroyFence(device.handle(), frame.Fence, nullptr);
        vkFreeCommandBuffers(device.handle(), frame.CommandPool, 1, &frame.CommandBuffer);
        vkDestroyCommandPool(device.handle(), frame.CommandPool, nullptr);
        vkDestroyImageView(device.handle(), frame.BackbufferView, nullptr);
        vkDestroyFramebuffer(device.handle(), frame.Framebuffer, nullptr);
    }
    for (int index = 0; index < g_MainWindowData.FrameSemaphores.Size; ++index)
    {
        ImGui_ImplVulkanH_FrameSemaphores& semaphores = g_MainWindowData.FrameSemaphores[index];
        vkDestroySemaphore(device.handle(), semaphores.ImageAcquiredSemaphore, nullptr);
        vkDestroySemaphore(device.handle(), semaphores.RenderCompleteSemaphore, nullptr);
    }
    g_MainWindowData.Frames.clear();
    g_MainWindowData.FrameSemaphores.clear();
    vkDestroyPipeline(device.handle(), g_MainWindowData.Pipeline, nullptr);
    vkDestroyRenderPass(device.handle(), g_MainWindowData.RenderPass, nullptr);
    vkDestroySwapchainKHR(device.handle(), g_MainWindowData.Swapchain, nullptr);
    g_MainWindowData = ImGui_ImplVulkanH_Window();
}
static void FrameRender(ImGui_ImplVulkanH_Window* wd, VkDevice device, VkQueue graphicsQueue, ImDrawData* draw_data)
{
    VkSemaphore image_acquired_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkResult err = vkAcquireNextImageKHR(device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE,
                                         &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);

    // Presentation waits are tied to acquired swapchain images, not in-flight frame slots.
    VkSemaphore render_complete_semaphore = g_PresentSemaphores[wd->FrameIndex];
    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    {
        err = vkWaitForFences(device, 1, &fd->Fence, VK_TRUE,
                              UINT64_MAX);  // wait indefinitely instead of periodically checking
        check_vk_result(err);

        err = vkResetFences(device, 1, &fd->Fence);
        check_vk_result(err);
    }
    {
        err = vkResetCommandPool(device, fd->CommandPool, 0);
        check_vk_result(err);
        VkCommandBufferBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
        check_vk_result(err);
    }
    {
        VkRenderPassBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = wd->RenderPass;
        info.framebuffer = fd->Framebuffer;
        info.renderArea.extent.width = wd->Width;
        info.renderArea.extent.height = wd->Height;
        info.clearValueCount = 1;
        info.pClearValues = &wd->ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    // Record dear imgui primitives into command buffer
    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

    // Submit command buffer
    vkCmdEndRenderPass(fd->CommandBuffer);
    {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &image_acquired_semaphore;
        info.pWaitDstStageMask = &wait_stage;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &fd->CommandBuffer;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &render_complete_semaphore;

        err = vkEndCommandBuffer(fd->CommandBuffer);
        check_vk_result(err);
        err = vkQueueSubmit(graphicsQueue, 1, &info, fd->Fence);
        check_vk_result(err);
    }
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd, VkQueue presentQueue)
{
    if (g_SwapChainRebuild)
        return;
    VkSemaphore render_complete_semaphore = g_PresentSemaphores[wd->FrameIndex];
    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = vkQueuePresentKHR(presentQueue, &info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;  // Now we can use the next set of semaphores
}

// Main code
int main(int, char**)
{
    Incubator::Log::Registry::init();
    auto log = Incubator::Log::Registry::get("Demo");

    log.info("Engine starting");

    // Create platform window (WindowGlfw handles glfwInit + glfwCreateWindow internally)
    Incubator::Window::WindowDesc windowDesc;
    windowDesc.title = "Incubator Engine";
    windowDesc.width = 1280;
    windowDesc.height = 720;

    auto engineWindow = Incubator::Window::createPlatformWindow(windowDesc);
    auto nativeWindow = static_cast<GLFWwindow*>(engineWindow->getNativeHandle());

    const auto extensions = Incubator::Render::Vulkan::GlfwSurface::requiredInstanceExtensions();
#ifdef _DEBUG
    constexpr auto validationMode = Incubator::Render::Vulkan::ValidationMode::Optional;
#else
    constexpr auto validationMode = Incubator::Render::Vulkan::ValidationMode::Disabled;
#endif
    Incubator::Render::Vulkan::VulkanInstance instance(extensions, validationMode);
    Incubator::Render::Vulkan::GlfwSurface surface(instance, *engineWindow);
    Incubator::Render::Vulkan::VulkanDevice device(instance, surface);
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    // Create Framebuffers
    int w, h;
    engineWindow->getFramebufferSize(w, h);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    SetupVulkanWindow(wd, instance, surface, device, w, h);
    g_PresentSemaphores = createPresentSemaphores(device.handle(), wd->ImageCount);
    VkDescriptorPool descriptorPool = createImGuiDescriptorPool(device.handle());

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;   // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    // ImGui::StyleColorsLight();

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForVulkan(nativeWindow, true);
    ImGui_ImplVulkan_InitInfo init_info = {};
    // init_info.ApiVersion = VK_API_VERSION_1_3;              // Pass in your value of VkApplicationInfo::apiVersion,
    // otherwise will default to header version.
    init_info.Instance = instance.handle();
    init_info.PhysicalDevice = device.physicalDevice();
    init_info.Device = device.handle();
    init_info.QueueFamily = device.queueFamilies().graphics;
    init_info.Queue = device.graphicsQueue();
    init_info.PipelineCache = pipelineCache;
    init_info.DescriptorPool = descriptorPool;
    init_info.RenderPass = wd->RenderPass;
    init_info.Subpass = 0;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = wd->ImageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.Allocator = nullptr;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);

    // Load Fonts
    // - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use
    // ImGui::PushFont()/PopFont() to select them.
    // - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
    // - If the file cannot be loaded, the function will return a nullptr. Please handle those errors in your
    // application (e.g. use an assertion, or display an error and quit).
    // - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling
    // ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
    // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use Freetype for higher quality font rendering.
    // - Read 'docs/FONTS.md' for more instructions and details.
    // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double
    // backslash \\ !
    // io.Fonts->AddFontDefault();
    // io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    // io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
    // io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
    // io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
    // ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f, nullptr,
    // io.Fonts->GetGlyphRangesJapanese()); IM_ASSERT(font != nullptr);

    // Our state
    bool show_demo_window = true;
    bool show_another_window = false;
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    double g_FrameTime = 0.0;

    // Main loop
    while (!engineWindow->shouldClose())
    {
        auto frameStart = Incubator::Clock::now();

        // Poll and handle events (inputs, window resize, etc.)
        // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your
        // inputs.
        // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or
        // clear/overwrite your copy of the mouse data.
        // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or
        // clear/overwrite your copy of the keyboard data. Generally you may always pass all inputs to dear imgui, and
        // hide them from your application based on those two flags.
        engineWindow->pollEvents();

        // Resize swap chain?
        int fb_width, fb_height;
        engineWindow->getFramebufferSize(fb_width, fb_height);
        if (fb_width > 0 && fb_height > 0 &&
            (g_SwapChainRebuild || g_MainWindowData.Width != fb_width || g_MainWindowData.Height != fb_height))
        {
            ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
            check_vk_result(vkDeviceWaitIdle(device.handle()));
            destroyPresentSemaphores(device.handle(), g_PresentSemaphores);
            ImGui_ImplVulkanH_CreateOrResizeWindow(instance.handle(), device.physicalDevice(), device.handle(),
                                                   &g_MainWindowData, device.queueFamilies().graphics, nullptr,
                                                   fb_width, fb_height, g_MinImageCount);
            g_PresentSemaphores = createPresentSemaphores(device.handle(), g_MainWindowData.ImageCount);
            g_MainWindowData.FrameIndex = 0;
            g_SwapChainRebuild = false;
        }
        if (engineWindow->isIconified())
        {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        // Start the Dear ImGui frame
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // 1. Show the big demo window (Most of the sample code is in ImGui::ShowDemoWindow()! You can browse its code
        // to learn more about Dear ImGui!).
        if (show_demo_window)
            ImGui::ShowDemoWindow(&show_demo_window);

        // 2. Show a simple window that we create ourselves. We use a Begin/End pair to create a named window.
        {
            static float f = 0.0f;
            static int counter = 0;

            ImGui::Begin("Hello, world!");  // Create a window called "Hello, world!" and append into it.

            ImGui::Text("This is some useful text.");           // Display some text (you can use a format strings too)
            ImGui::Checkbox("Demo Window", &show_demo_window);  // Edit bools storing our window open/close state
            ImGui::Checkbox("Another Window", &show_another_window);

            ImGui::SliderFloat("float", &f, 0.0f, 1.0f);             // Edit 1 float using a slider from 0.0f to 1.0f
            ImGui::ColorEdit3("clear color", (float*)&clear_color);  // Edit 3 floats representing a color

            if (ImGui::Button(
                    "Button"))  // Buttons return true when clicked (most widgets return true when edited/activated)
                counter++;
            ImGui::SameLine();
            ImGui::Text("counter = %d", counter);

            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
            ImGui::Separator();
            ImGui::Text("Engine frame: %.3f ms (%.1f FPS)",
                        1000.0 / g_FrameTime, 1.0 / g_FrameTime);
            ImGui::End();
        }

        // 3. Show another simple window.
        if (show_another_window)
        {
            ImGui::Begin("Another Window",
                         &show_another_window);  // Pass a pointer to our bool variable (the window will have a closing
                                                 // button that will clear the bool when clicked)
            ImGui::Text("Hello from another window!");
            if (ImGui::Button("Close Me"))
                show_another_window = false;
            ImGui::End();
        }

        // Rendering
        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool is_minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
        if (!is_minimized)
        {
            wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
            wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
            wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
            wd->ClearValue.color.float32[3] = clear_color.w;
            FrameRender(wd, device.handle(), device.graphicsQueue(), draw_data);
            FramePresent(wd, device.presentQueue());
        }

        g_FrameTime = Incubator::Clock::elapsedSeconds(frameStart);
    }

    // Cleanup
    log.info("Engine shutting down");
    VkResult err = vkDeviceWaitIdle(device.handle());
    check_vk_result(err);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    CleanupVulkanWindow(device);
    destroyPresentSemaphores(device.handle(), g_PresentSemaphores);
    vkDestroyDescriptorPool(device.handle(), descriptorPool, nullptr);

    // Local Vulkan objects are destroyed in reverse order before engineWindow.

    log.info("Engine shutdown complete");

    Incubator::Log::Registry::flushAll();
    return 0;
}
