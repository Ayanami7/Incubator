#include "runtime/resource/loader/shader_loader.h"

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/platform/fileio/fileio.h"
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
            testDirectory_ =
                fs::temp_directory_path() /
                ("incubator-shader-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            fs::create_directories(testDirectory_ / "assets");
            fs::current_path(testDirectory_);
            fileIO_ = FileIO::createPlatformFileIO();
            source_ = std::make_unique<AssetSource>(*fileIO_);
            Log::Registry::reset();
            Log::Registry::get("resource").addSink(std::make_shared<spdlog::sinks::ostream_sink_mt>(messages_));
        }

        void TearDown() override
        {
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            Log::Registry::reset();
            source_.reset();
            fileIO_.reset();
            fs::current_path(previousDirectory_);
            fs::remove_all(testDirectory_);
        }

        void writeWords(const std::vector<uint32_t>& words, std::string_view relativePath = "shaders/test.spv")
        {
            const auto path = fs::path("assets") / Utils::Utf8::toFsPath(relativePath);
            fs::create_directories(path.parent_path());
            std::ofstream output(path, std::ios::binary);
            output.write(reinterpret_cast<const char*>(words.data()),
                         static_cast<std::streamsize>(words.size() * sizeof(uint32_t)));
            ASSERT_TRUE(output.good());
        }

        void registerShaderLoader()
        {
            ResourceManager::instance().registerLoader(ResourceType::SHADER, std::make_unique<ShaderLoader>(*source_));
        }

        fs::path previousDirectory_;
        fs::path testDirectory_;
        std::unique_ptr<FileIO::FileIO> fileIO_;
        std::unique_ptr<AssetSource> source_;
        std::ostringstream messages_;
    };

    TEST_F(ShaderLoaderTest, LoadsOwnedPayloadAndUnloadsAtZeroReferences)
    {
        const std::vector<uint32_t> words = {0x07230203, 0x00010600, 0, 1, 0};
        writeWords(words);
        registerShaderLoader();
        auto& manager = ResourceManager::instance();
        auto first = manager.hold<ShaderResource>(ResourceType::SHADER, "shaders/test.spv");
        ASSERT_TRUE(first);
        const auto id = first.id();
        const auto* shader = first.get<ShaderResource>();
        ASSERT_NE(shader, nullptr);
        EXPECT_EQ(shader->words(), words);
        auto second = manager.hold<ShaderResource>(ResourceType::SHADER, "shaders/test.spv");
        ASSERT_TRUE(second);
        EXPECT_EQ(second.get<ShaderResource>(), shader);
        second.reset();
        first.reset();
        EXPECT_EQ(ResourceRegistry::instance().getObject(id), nullptr);
    }

    TEST_F(ShaderLoaderTest, LoadsShaderFromChinesePath)
    {
        const std::vector<uint32_t> words = {0x07230203, 0x00010600, 0, 1, 0};
        writeWords(words, "着色器/测试.spv");
        registerShaderLoader();
        const auto hold = ResourceManager::instance().hold<ShaderResource>(ResourceType::SHADER, "着色器/测试.spv");
        ASSERT_TRUE(hold);
        const auto* shader = hold.get<ShaderResource>();
        ASSERT_NE(shader, nullptr);
        EXPECT_EQ(shader->words(), words);
    }

    TEST_F(ShaderLoaderTest, DirectExceptionIsLoggedOnceByManagerAndConvertedToNull)
    {
        registerShaderLoader();
        const auto id = ResourceId::fromPath(ResourceType::SHADER, "shaders/test.spv");
        ShaderLoader directLoader(*source_);
        EXPECT_THROW(directLoader.load(id, "shaders/test.spv"), Error::Exception);
        EXPECT_TRUE(messages_.str().empty());
        const auto hold = ResourceManager::instance().hold<ShaderResource>(ResourceType::SHADER, "shaders/test.spv");
        EXPECT_FALSE(hold);
        EXPECT_EQ(ResourceRegistry::instance().getObject(id), nullptr);
        const auto messages = messages_.str();
        const auto first = messages.find("load: loader threw:");
        ASSERT_NE(first, std::string::npos);
        EXPECT_EQ(messages.find("load: loader threw:", first + 1), std::string::npos);
        EXPECT_NE(messages.find("SPIR-V file was not found"), std::string::npos);
    }

    TEST_F(ShaderLoaderTest, MissingChinesePathIsPreservedInDiagnostic)
    {
        const std::string virtualPath = "着色器/缺失.spv";
        registerShaderLoader();
        const auto hold = ResourceManager::instance().hold<ShaderResource>(ResourceType::SHADER, virtualPath);
        EXPECT_FALSE(hold);
        EXPECT_NE(messages_.str().find("assets/着色器/缺失.spv"), std::string::npos);
        EXPECT_NE(messages_.str().find("SPIR-V file was not found"), std::string::npos);
    }

    TEST_F(ShaderLoaderTest, InvalidPayloadFailsWithoutMountingAndCanBeRetried)
    {
        writeWords({0, 0, 0, 0, 0});
        registerShaderLoader();
        auto& manager = ResourceManager::instance();
        const auto id = ResourceId::fromPath(ResourceType::SHADER, "shaders/test.spv");
        EXPECT_FALSE(manager.hold<ShaderResource>(ResourceType::SHADER, "shaders/test.spv"));
        EXPECT_EQ(ResourceRegistry::instance().getObject(id), nullptr);
        EXPECT_NE(messages_.str().find("Invalid SPIR-V magic number"), std::string::npos);
        const std::vector<uint32_t> valid = {0x07230203, 0x00010600, 0, 1, 0};
        writeWords(valid);
        const auto hold = manager.hold<ShaderResource>(ResourceType::SHADER, "shaders/test.spv");
        ASSERT_TRUE(hold);
        EXPECT_EQ(hold.get<ShaderResource>()->words(), valid);
    }

    TEST_F(ShaderLoaderTest, InvalidChinesePathPayloadPreservesPathAndReason)
    {
        writeWords({0, 0, 0, 0, 0}, "着色器/坏数据.spv");
        registerShaderLoader();
        EXPECT_FALSE(ResourceManager::instance().hold<ShaderResource>(ResourceType::SHADER, "着色器/坏数据.spv"));
        EXPECT_NE(messages_.str().find("assets/着色器/坏数据.spv"), std::string::npos);
        EXPECT_NE(messages_.str().find("Invalid SPIR-V magic number"), std::string::npos);
    }
}  // namespace
