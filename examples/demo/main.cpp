#include <array>
#include <chrono>
#include <exception>
#include <thread>

#include <imgui.h>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/platform/window/window.h"
#include "runtime/render/integration/imgui_renderer.h"
#include "runtime/render/renderer.h"

int main()
{
    Incubator::Log::Registry::init();
    auto log = Incubator::Log::Registry::get("Demo");
    log.info("Engine starting");

    try
    {
        Incubator::Window::WindowDesc windowDesc;
        windowDesc.title = "Incubator Engine";
        windowDesc.width = 1280;
        windowDesc.height = 720;
        auto window = Incubator::Window::createPlatformWindow(windowDesc);

#ifdef _DEBUG
        constexpr auto validationMode = Incubator::Render::RendererValidationMode::Optional;
#else
        constexpr auto validationMode = Incubator::Render::RendererValidationMode::Disabled;
#endif
        Incubator::Render::Renderer renderer(*window, validationMode);
        std::array<float, 4> clearColor = {0.12f, 0.16f, 0.22f, 1.0f};

        while (!window->shouldClose())
        {
            window->pollEvents();
            renderer.imgui().beginFrame();
            ImGui::Begin("Incubator Demo");
            ImGui::Text("Vulkan textured cube");
            ImGui::ColorEdit3("Clear color", clearColor.data());
            ImGui::Text("Application controls events; Renderer owns frame submission.");
            ImGui::End();
            renderer.imgui().endFrame();

            void* uiDrawData = renderer.imgui().drawData();
            renderer.drawFrame(clearColor, [&renderer, uiDrawData](void* commandBuffer) {
                renderer.imgui().recordDrawData(uiDrawData, commandBuffer);
            });
            if (window->isIconified())
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    catch (const Incubator::Error::Exception& error)
    {
        log.error("Engine error: {}", error.what());
        return 1;
    }
    catch (const std::exception& error)
    {
        log.error("Unexpected error: {}", error.what());
        return 1;
    }

    log.info("Engine shutdown complete");
    return 0;
}
