#include <exception>
#include <filesystem>
#include <utility>

#include "app/common/startup_config.h"
#include "app/common/validation_mode.h"
#include "editor_app.h"
#include "editor_scene.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"

int main()
{
    using namespace Incubator;
    Log::Registry::init(Log::Level::DEBUG);
    auto log = Log::Registry::get("editor");
    try
    {
        log.info("Editor working directory: {}", Utils::Utf8::toUtf8String(std::filesystem::current_path()));
        auto fileIO = FileIO::createPlatformFileIO();
        auto config = App::readStartupConfig(*fileIO);
        log.info("Startup scene: {}", config.startupScene);
        config.options.validation = App::VALIDATION_MODE;
        Editor::EditorApp app(config.options, std::move(fileIO));
        app.run(Editor::makeCallbacks(config.startupScene));
    }
    catch (const Model::LoadError&)
    {
        // 资源门面已经记录模型或依赖的具体加载原因。
        return 1;
    }
    catch (const std::exception& error)
    {
        log.error("Editor failed: {}", error.what());
        return 1;
    }
    log.info("Editor shutdown complete");
    return 0;
}
