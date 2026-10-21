#include <exception>

#include "../demo/validation_mode.h"
#include "runtime/core/log/logger.h"
#include "scene_demo.h"

int main()
{
    using namespace Incubator;
    Log::Registry::init(Log::Level::DEBUG);
    auto log = Log::Registry::get("scene_demo");
    try
    {
        Application::SessionOptions options;
        options.window.title = "Incubator Scene Runtime";
        options.window.width = 1280;
        options.window.height = 720;
        options.validation = Demo::VALIDATION_MODE;
        Application::Session session(options);
        session.run(SceneDemo::makeCallbacks());
    }
    catch (const std::exception& error)
    {
        log.error("Scene application failed: {}", error.what());
        return 1;
    }
    log.info("Scene application shutdown complete");
    return 0;
}
