#include <array>
#include <chrono>
#include <exception>
#include <thread>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/platform/window/window.h"
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
        constexpr std::array<float, 4> clearColor = {0.12f, 0.16f, 0.22f, 1.0f};

        while (!window->shouldClose())
        {
            window->pollEvents();
            renderer.drawFrame(clearColor);
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
