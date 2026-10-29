#include "runtime/resource/asset_source.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "runtime/core/error/error.h"
#include "runtime/resource/loader/text_loader.h"
#include "runtime/resource/loader/text_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource;
    namespace fs = std::filesystem;

    class AssetSourceTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            cwd_ = fs::current_path();
            directory_ = cwd_ / "review/asset-source" / ::testing::UnitTest::GetInstance()->current_test_info()->name();
            fs::create_directories(directory_ / "A");
            fs::create_directories(directory_ / "B");
            std::ofstream(directory_ / "A/value.txt") << "root A";
            std::ofstream(directory_ / "B/value.txt") << "root B";
            fileIO_ = FileIO::createPlatformFileIO();
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            ResourceManager::instance().bindAssetSource(nullptr);
        }
        void TearDown() override
        {
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            ResourceManager::instance().bindAssetSource(nullptr);
            EXPECT_EQ(fs::current_path(), cwd_);
        }
        fs::path cwd_, directory_;
        std::unique_ptr<FileIO::FileIO> fileIO_;
    };

    TEST_F(AssetSourceTest, CapturesAbsoluteRootAndRejectsNonRelativeOrEscapingFilePaths)
    {
        AssetSource defaults(*fileIO_);
        EXPECT_EQ(defaults.root(), (cwd_ / "assets").lexically_normal());
        AssetSource source(*fileIO_, directory_ / "A");
        EXPECT_EQ(source.resolve("parent/../value.txt"), directory_ / "A/value.txt");
        EXPECT_TRUE(source.exists("value.txt"));
        EXPECT_FALSE(source.exists("missing.txt"));
        for (const auto path : {"", ".", "..", "a/../../value.txt", "/value.txt", "\\value.txt", "C:value.txt",
                                "C:/value.txt", "//server/value.txt", "https://host/file"})
            EXPECT_THROW(source.open(path), Error::Exception) << path;
        EXPECT_THROW(source.resolve(std::string("a\0b", 3)), Error::Exception);
        EXPECT_THROW(source.resolve(std::string("\xff", 1)), Error::Exception);
    }

    TEST_F(AssetSourceTest, RejectsLiveSourceChangesThenReloadsSameIdentityFromNewRoot)
    {
        AssetSource a(*fileIO_, directory_ / "A"), b(*fileIO_, directory_ / "B");
        auto& manager = ResourceManager::instance();
        auto& registry = ResourceRegistry::instance();
        manager.bindAssetSource(&a);
        manager.registerLoader(ResourceType::TEXT, std::make_unique<TextLoader>(a));
        const auto id = manager.registerResource(ResourceType::TEXT, "value.txt");
        const auto* original = manager.get<TextResource>(id);
        ASSERT_NE(original, nullptr);
        EXPECT_EQ(original->text(), "root A");
        EXPECT_THROW(manager.bindAssetSource(&b), Error::Exception);
        manager.reset();
        EXPECT_THROW(manager.bindAssetSource(nullptr), Error::Exception);
        EXPECT_THROW(manager.bindAssetSource(&b), Error::Exception);
        EXPECT_EQ(manager.assetSource(), &a);
        EXPECT_EQ(manager.get<TextResource>(id), original);
        manager.release(id);
        EXPECT_FALSE(registry.hasLiveResources());
        manager.bindAssetSource(nullptr);
        manager.bindAssetSource(&b);
        manager.registerLoader(ResourceType::TEXT, std::make_unique<TextLoader>(b));
        EXPECT_EQ(manager.registerResource(ResourceType::TEXT, "value.txt"), id);
        manager.acquire(id);  // 已有零持有登记需要显式取得本次持有。
        ASSERT_NE(manager.get<TextResource>(id), nullptr);
        EXPECT_EQ(manager.get<TextResource>(id)->text(), "root B");
        manager.release(id);
        manager.reset();
        manager.bindAssetSource(nullptr);
    }

    TEST_F(AssetSourceTest, UnownedMountedPayloadAlsoPreventsSourceDetachment)
    {
        AssetSource source(*fileIO_, directory_ / "A");
        auto& manager = ResourceManager::instance();
        auto& registry = ResourceRegistry::instance();
        manager.bindAssetSource(&source);
        manager.registerLoader(ResourceType::TEXT, std::make_unique<TextLoader>(source));
        const auto id = manager.registerResource(ResourceType::TEXT, "value.txt");
        ASSERT_NE(manager.load(id), nullptr);
        registry.release(id);  // Registry 允许零持有且挂载，Manager 解除绑定仍须拒绝。
        manager.reset();
        EXPECT_THROW(manager.bindAssetSource(nullptr), Error::Exception);
        EXPECT_EQ(manager.assetSource(), &source);
        registry.detachObject(id);
        manager.bindAssetSource(nullptr);
    }
}  // namespace
