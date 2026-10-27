#include "examples/demo/startup_config.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "runtime/core/error/error.h"
#include "runtime/core/utils/utf8.h"

namespace
{
    using namespace Incubator;

    TEST(StartupConfigTest, DefaultsAndPartialFieldsLeaveOtherOptionsUnchanged)
    {
        const auto defaults = Demo::decodeStartupConfig("{}");
        EXPECT_EQ(defaults.options.window.title, "Incubator Engine Demo");
        EXPECT_EQ(defaults.options.window.width, 1280);
        EXPECT_EQ(defaults.options.window.height, 720);
        EXPECT_EQ(defaults.startupScene, "assets/scenes/mixed.json");
        const auto partial = Demo::decodeStartupConfig(R"({"window":{"width":900},"unknown":true})");
        EXPECT_EQ(partial.options.window.width, 900);
        EXPECT_EQ(partial.options.window.height, defaults.options.window.height);
        EXPECT_EQ(partial.options.vertexShader, defaults.options.vertexShader);
        EXPECT_EQ(partial.options.validation, defaults.options.validation);
        EXPECT_EQ(partial.options.clearColor, defaults.options.clearColor);
    }

    TEST(StartupConfigTest, ReadsUtf8TitleAndScenePathWithoutRebasing)
    {
        const auto config = Demo::decodeStartupConfig(
            R"({"window":{"title":"场景编辑","width":960,"height":540},"startupScene":"场景/主场景.json"})");
        EXPECT_EQ(config.options.window.title, "场景编辑");
        EXPECT_EQ(config.options.window.width, 960);
        EXPECT_EQ(config.options.window.height, 540);
        EXPECT_EQ(config.startupScene, "场景/主场景.json");
    }

    TEST(StartupConfigTest, RejectsInvalidFieldsWithContext)
    {
        const std::pair<std::string_view, std::string_view> cases[] = {
            {"[]", "JSON"}, {"{", "JSON"}, {"{}{}", "JSON"},
            {R"({"window":null})", "window"}, {R"({"window":{"width":0}})", "window.width"},
            {R"({"window":{"width":2147483648}})", "window.width"},
            {R"({"window":{"height":-1}})", "window.height"},
            {R"({"window":{"height":720.0}})", "window.height"},
            {R"({"window":{"title":1}})", "window.title"},
            {R"({"window":{"title":"a\u0000b"}})", "window.title"},
            {R"({"startupScene":null})", "startupScene"}, {R"({"startupScene":""})", "startupScene"},
            {R"({"startupScene":"a\u0000b"})", "startupScene"},
            {std::string_view("{}\0{}", 5), "JSON"}, {"\xff", "JSON"}};
        for (const auto& [json, field] : cases)
        {
            SCOPED_TRACE(json);
            try
            {
                Demo::decodeStartupConfig(json);
                FAIL() << "accepted invalid config";
            }
            catch (const Error::Exception& error)
            {
                EXPECT_EQ(error.code(), Error::Code::CONFIG);
                EXPECT_NE(error.message().find(field), std::string::npos);
            }
        }
    }

    TEST(StartupConfigTest, FileReadsUseDefaultsOnlyWhenMissingAndKeepWorkingDirectory)
    {
        namespace fs = std::filesystem;
        const auto cwd = fs::current_path();
        const auto directory = cwd / "review/startup-config";
        fs::create_directories(directory);
        const auto path = directory / Utils::Utf8::toFsPath("启动.json");
        const auto utf8 = Utils::Utf8::toUtf8String(path);
        auto io = FileIO::createPlatformFileIO();
        fs::remove(path);
        EXPECT_EQ(Demo::readStartupConfig(*io, utf8).startupScene, "assets/scenes/mixed.json");
        {
            std::ofstream file(path, std::ios::binary);
            file << R"({"startupScene":"assets/scenes/主场景.json"})";
        }
        EXPECT_EQ(Demo::readStartupConfig(*io, utf8).startupScene, "assets/scenes/主场景.json");
        {
            std::ofstream file(path, std::ios::binary);
            file << R"({"window":{"width":0}})";
        }
        try
        {
            Demo::readStartupConfig(*io, utf8);
            FAIL() << "accepted invalid config file";
        }
        catch (const Error::Exception& error)
        {
            EXPECT_EQ(error.code(), Error::Code::CONFIG);
            EXPECT_NE(error.message().find(utf8), std::string::npos);
            EXPECT_NE(error.message().find("window.width"), std::string::npos);
        }
        EXPECT_THROW(Demo::readStartupConfig(*io, Utils::Utf8::toUtf8String(directory)), Error::Exception);
        EXPECT_EQ(fs::current_path(), cwd);
        fs::remove(path);
    }
}  // namespace
