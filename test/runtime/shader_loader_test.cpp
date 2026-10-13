#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/resource/loader/shader_loader.h"
#include "runtime/resource/loader/shader_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"

namespace
{
    namespace fs = std::filesystem;
    using namespace Incubator;
    using namespace Incubator::Resource;

    class ShaderLoaderTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            previousDirectory_ = fs::current_path();
            testDirectory_ = fs::temp_directory_path() /
                             ("incubator-shader-" + std::to_string(
                                 std::chrono::steady_clock::now().time_since_epoch().count()));
            fs::create_directories(testDirectory_ / "assets/shaders");
            fs::current_path(testDirectory_);
            fileIO_ = FileIO::createPlatformFileIO();
            Log::Registry::reset();
            Log::Registry::get("resource").addSink(std::make_shared<spdlog::sinks::ostream_sink_mt>(messages_));
        }

        void TearDown() override
        {
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            Log::Registry::reset();
            fileIO_.reset();
            fs::current_path(previousDirectory_);
            fs::remove_all(testDirectory_);
        }

        void writeWords(const std::vector<uint32_t>& words)
        {
            std::ofstream output("assets/shaders/test.spv", std::ios::binary);
            output.write(reinterpret_cast<const char*>(words.data()),
                         static_cast<std::streamsize>(words.size() * sizeof(uint32_t)));
            ASSERT_TRUE(output.good());
        }

        ResourceId registerShader()
        {
            auto& manager = ResourceManager::instance();
            manager.registerLoader(ResourceType::SHADER, std::make_unique<ShaderLoader>(*fileIO_));
            return manager.registerResource(ResourceType::SHADER, "shaders/test.spv");
        }

        fs::path previousDirectory_;
        fs::path testDirectory_;
        std::unique_ptr<FileIO::FileIO> fileIO_;
        std::ostringstream messages_;
    };

    TEST_F(ShaderLoaderTest, LoadsOwnedPayloadAndUnloadsAtZeroReferences)
    {
        const std::vector<uint32_t> words = {0x07230203, 0x00010600, 0, 1, 0};
        writeWords(words);
        const auto id = registerShader();
        auto& manager = ResourceManager::instance();
        auto* shader = manager.get<ShaderResource>(id);
        ASSERT_NE(shader, nullptr);
        EXPECT_EQ(shader->words(), words);
        EXPECT_EQ(shader->state(), ResourceState::READY);
        EXPECT_EQ(manager.get<ShaderResource>(id), shader);
        manager.release(id);
        EXPECT_EQ(ResourceRegistry::instance().getObject(id), nullptr);
    }

    TEST_F(ShaderLoaderTest, DirectExceptionIsLoggedOnceByManagerAndConvertedToNull)
    {
        const auto id = registerShader();
        ShaderLoader directLoader(*fileIO_);
        EXPECT_THROW(directLoader.load(id, "shaders/test.spv"), Error::Exception);
        EXPECT_TRUE(messages_.str().empty());
        EXPECT_EQ(ResourceManager::instance().load(id), nullptr);
        EXPECT_EQ(ResourceRegistry::instance().getObject(id), nullptr);
        const auto messages = messages_.str();
        const auto first = messages.find("load: loader threw:");
        ASSERT_NE(first, std::string::npos);
        EXPECT_EQ(messages.find("load: loader threw:", first + 1), std::string::npos);
        EXPECT_NE(messages.find("SPIR-V file was not found"), std::string::npos);
    }

    TEST_F(ShaderLoaderTest, InvalidPayloadFailsWithoutMountingAndCanBeRetried)
    {
        writeWords({0, 0, 0, 0, 0});
        const auto id = registerShader();
        auto& manager = ResourceManager::instance();
        EXPECT_EQ(manager.load(id), nullptr);
        EXPECT_EQ(ResourceRegistry::instance().getObject(id), nullptr);
        EXPECT_NE(messages_.str().find("Invalid SPIR-V magic number"), std::string::npos);
        const std::vector<uint32_t> valid = {0x07230203, 0x00010600, 0, 1, 0};
        writeWords(valid);
        auto* shader = manager.get<ShaderResource>(id);
        ASSERT_NE(shader, nullptr);
        EXPECT_EQ(shader->words(), valid);
    }
}
