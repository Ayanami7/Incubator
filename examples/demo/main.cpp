#include <imgui.h>

#include <array>
#include <chrono>
#include <exception>
#include <thread>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/platform/window/window.h"
#include "runtime/render/integration/imgui_renderer.h"
#include "runtime/render/renderer.h"
#include "runtime/resource/resource_registry.h"
#include "scene_assets.h"
#include "validation_mode.h"

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

        Incubator::Demo::SceneAssets assets;
        const auto textureId = assets.texture->id();
        Incubator::Render::Renderer renderer(*window, *assets.vertex, *assets.fragment, *assets.texture,
                                             Incubator::Demo::VALIDATION_MODE);
        assets.releaseCpuResources();
        if (Incubator::Resource::ResourceRegistry::instance().getObject(textureId))
            throw Incubator::Error::Exception(Incubator::Error::Code::RENDER, "CPU scene texture was not unloaded");
        std::array<float, 4> clearColor = {0.12f, 0.16f, 0.22f, 1.0f};

        while (!window->shouldClose())
        {
            window->pollEvents();
            if (!renderer.beginUiFrame())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
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
