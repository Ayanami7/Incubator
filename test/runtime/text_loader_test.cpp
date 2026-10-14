#define TESTING_ENABLED

#include "runtime/resource/loader/text_loader.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/resource/loader/text_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"

using namespace Incubator::Resource;

namespace fs = std::filesystem;

namespace
{
    class UnrelatedResource final : public ResourceObject
    {
    public:
        UnrelatedResource(ResourceId id, ResourceType type) : ResourceObject(id, type) {}
    };

    class CountingLoader final : public LoaderBase
    {
    public:
        CountingLoader(std::unique_ptr<LoaderBase> inner, int* callCount)
            : inner_(std::move(inner)), callCount_(callCount)
        {
        }

        std::unique_ptr<ResourceObject> load(const ResourceId& id,
                                             std::string_view virtualPath) override
        {
            ++(*callCount_);
            return inner_->load(id, virtualPath);
        }

    private:
        std::unique_ptr<LoaderBase> inner_;
        int* callCount_;
    };
}  // namespace

class TextLoaderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        originalCwd_ = fs::current_path();

        testDir_ = fs::temp_directory_path() / "IncubatorTextLoaderTest";
        testDir_ /= std::to_string(reinterpret_cast<uintptr_t>(this));
        fs::remove_all(testDir_);
        fs::create_directories(testDir_);

        // TextLoader 将虚拟路径 join 为相对 "assets/..." 路径，真实 FileIO 相对进程工作目录解析，
        // 因此测试期间把工作目录切换到临时目录，让 join 后的路径落在临时目录下。
        fs::current_path(testDir_);

        fileIO_ = Incubator::FileIO::createPlatformFileIO();
        ASSERT_NE(fileIO_, nullptr);

        ResourceManager::instance().reset();
        ResourceRegistry::instance().clear();
        Incubator::Log::Registry::reset();
        Incubator::Log::Registry::get("resource").addSink(
            std::make_shared<spdlog::sinks::ostream_sink_mt>(messages_));
    }

    void TearDown() override
    {
        ResourceManager::instance().reset();
        ResourceRegistry::instance().clear();
        Incubator::Log::Registry::reset();

        fileIO_.reset();
        fs::current_path(originalCwd_);

        std::error_code ec;
        fs::remove_all(testDir_, ec);
    }

    void createAssetFile(std::string_view relativePath, const std::string& content)
    {
        fs::path full = testDir_ / "assets" / Incubator::Utils::Utf8::toFsPath(relativePath);
        fs::create_directories(full.parent_path());
        std::ofstream ofs(full, std::ios::binary);
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    void registerTextLoader()
    {
        ResourceManager::instance().registerLoader(
            ResourceType::TEXT, std::make_unique<TextLoader>(*fileIO_));
    }

    fs::path originalCwd_;
    fs::path testDir_;
    std::unique_ptr<Incubator::FileIO::FileIO> fileIO_;
    std::ostringstream messages_;
};

TEST_F(TextLoaderTest, FullChainLoadsTextFile)
{
    createAssetFile("settings/config.txt", "hello config");

    registerTextLoader();
    auto& manager = ResourceManager::instance();

    ResourceId id = manager.registerResource(ResourceType::TEXT, "settings/config.txt");
    ASSERT_TRUE(id.valid());

    TextResource* res = manager.get<TextResource>(id);
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->id(), id);
    EXPECT_EQ(res->type(), ResourceType::TEXT);
    EXPECT_EQ(res->state(), ResourceState::READY);
    EXPECT_EQ(res->text(), "hello config");
}

TEST_F(TextLoaderTest, VirtualPathResolvesUnderAssetsRoot)
{
    createAssetFile("shaders/debug.frag", "void main() {}");

    registerTextLoader();
    auto& manager = ResourceManager::instance();

    ResourceId id = manager.registerResource(ResourceType::TEXT, "shaders/debug.frag");
    ASSERT_TRUE(id.valid());

    TextResource* res = manager.get<TextResource>(id);
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->text(), "void main() {}");
}

TEST_F(TextLoaderTest, LoadsTextFromChinesePath)
{
    createAssetFile("素材/角色.txt", "中文资源内容");

    registerTextLoader();
    auto& manager = ResourceManager::instance();
    ResourceId id = manager.registerResource(ResourceType::TEXT, "素材/角色.txt");

    ASSERT_TRUE(id.valid());
    TextResource* res = manager.get<TextResource>(id);
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->text(), "中文资源内容");
}

TEST_F(TextLoaderTest, MissingChinesePathIsPreservedInDiagnostic)
{
    registerTextLoader();
    auto& manager = ResourceManager::instance();
    ResourceId id = manager.registerResource(ResourceType::TEXT, "素材/缺失.txt");

    ASSERT_TRUE(id.valid());
    EXPECT_EQ(manager.load(id), nullptr);
    EXPECT_NE(messages_.str().find("assets/素材/缺失.txt"), std::string::npos);
}

TEST_F(TextLoaderTest, SecondLoadReusesMountedObject)
{
    createAssetFile("data/once.txt", "loaded once");

    int loaderCalls = 0;
    ResourceManager::instance().registerLoader(
        ResourceType::TEXT,
        std::make_unique<CountingLoader>(std::make_unique<TextLoader>(*fileIO_), &loaderCalls));
    auto& manager = ResourceManager::instance();

    ResourceId id = manager.registerResource(ResourceType::TEXT, "data/once.txt");
    ASSERT_TRUE(id.valid());

    ResourceObject* first = manager.load(id);
    ASSERT_NE(first, nullptr);

    ResourceObject* second = manager.load(id);
    EXPECT_EQ(second, first);
    EXPECT_EQ(loaderCalls, 1);
}

TEST_F(TextLoaderTest, MissingFileReturnsNull)
{
    registerTextLoader();
    auto& manager = ResourceManager::instance();

    ResourceId id = manager.registerResource(ResourceType::TEXT, "settings/missing.txt");
    ASSERT_TRUE(id.valid());

    EXPECT_EQ(manager.load(id), nullptr);
    EXPECT_EQ(ResourceRegistry::instance().getObject(id), nullptr);
}

TEST_F(TextLoaderTest, NoLoaderReturnsNull)
{
    auto& manager = ResourceManager::instance();
    ResourceId id = manager.registerResource(ResourceType::TEXT, "settings/config.txt");
    ASSERT_TRUE(id.valid());

    EXPECT_EQ(manager.load(id), nullptr);
}

TEST_F(TextLoaderTest, GetWithWrongTypeReturnsNull)
{
    createAssetFile("settings/config.txt", "hello config");

    registerTextLoader();
    auto& manager = ResourceManager::instance();

    ResourceId id = manager.registerResource(ResourceType::TEXT, "settings/config.txt");
    ASSERT_TRUE(id.valid());

    EXPECT_EQ(manager.get<UnrelatedResource>(id), nullptr);
}

TEST_F(TextLoaderTest, ReleaseToZeroUnloadsButRetainsRegistration)
{
    createAssetFile("settings/config.txt", "hello config");

    registerTextLoader();
    auto& manager = ResourceManager::instance();

    ResourceId id = manager.registerResource(ResourceType::TEXT, "settings/config.txt");
    ASSERT_TRUE(id.valid());
    ASSERT_NE(manager.load(id), nullptr);

    manager.release(id);

    EXPECT_EQ(ResourceRegistry::instance().refCount(id), 0u);
    EXPECT_EQ(ResourceRegistry::instance().getObject(id), nullptr);

    ResourceType type = ResourceType::INVALID;
    std::string path;
    ASSERT_TRUE(ResourceRegistry::instance().lookup(id, type, path));
    EXPECT_EQ(type, ResourceType::TEXT);
    EXPECT_EQ(ResourceRegistry::instance().lookup(ResourceType::TEXT, "settings/config.txt"), id);
}

TEST_F(TextLoaderTest, ReloadAfterUnloadRebuildsObject)
{
    createAssetFile("settings/reloadable.txt", "v1");

    registerTextLoader();
    auto& manager = ResourceManager::instance();

    ResourceId id = manager.registerResource(ResourceType::TEXT, "settings/reloadable.txt");
    ASSERT_TRUE(id.valid());
    ASSERT_NE(manager.load(id), nullptr);

    manager.release(id);
    EXPECT_EQ(ResourceRegistry::instance().getObject(id), nullptr);

    TextResource* reloaded = manager.get<TextResource>(id);
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->text(), "v1");
}
