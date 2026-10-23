#include <exception>

#include "demo_app.h"
#include "runtime/core/log/logger.h"
#include "validation_mode.h"

int main()
{
    using namespace Incubator;
    Log::Registry::init(Log::Level::DEBUG);
    auto log = Log::Registry::get("demo");
    try
    {
        Demo::DemoOptions options;
        options.window.title = "Incubator Engine Demo";
        options.window.width = 1280;
        options.window.height = 720;
        options.validation = Demo::VALIDATION_MODE;
        Demo::DemoApp app(options);
        app.run();
    }
    catch (const std::exception& error)
    {
        log.error("Demo failed: {}", error.what());
        return 1;
    }
    log.info("Demo shutdown complete");
    return 0;
}