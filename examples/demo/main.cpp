#include <exception>
#include <utility>

#include "demo_app.h"
#include "model_scene.h"
#include "runtime/core/log/logger.h"
#include "startup_config.h"
#include "validation_mode.h"

int main()
{
    using namespace Incubator;
    Log::Registry::init(Log::Level::DEBUG);
    auto log = Log::Registry::get("demo");
    try
    {
        auto fileIO = FileIO::createPlatformFileIO();
        auto config = Demo::readStartupConfig(*fileIO);
        config.options.validation = Demo::VALIDATION_MODE;
        Demo::DemoApp app(config.options, std::move(fileIO));
        app.run(Demo::makeCallbacks(config.startupScene));
    }
    catch (const std::exception& error)
    {
        log.error("Demo failed: {}", error.what());
        return 1;
    }
    log.info("Demo shutdown complete");
    return 0;
}
