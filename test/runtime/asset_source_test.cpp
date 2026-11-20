#include "runtime/resource/asset_source.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "runtime/core/error/error.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource;
    namespace fs = std::filesystem;
    constexpr auto TEST_TEXT_TYPE = static_cast<ResourceType>(0x1234);

    class StubTextResource final : public ResourceObject
    {
    public:
        StubTextResource(ResourceId id, std::string text) : ResourceObject(id, TEST_TEXT_TYPE), text_(std::move(text)) {}
        const std::string& text() const noexcept
        {
            return text_;
        }

    private:
        std::string text_;
    };

    class StubTextLoader final : public LoaderBase
    {
    public:
        explicit StubTextLoader(AssetSource& source) : source_(source) {}

        std::unique_ptr<ResourceObject> load(const ResourceId& id, std::string_view virtualPath) override
        {
            auto handle = source_.open(virtualPath);
            if (!handle)
                return nullptr;
            std::string text(static_cast<size_t>(handle->size()), '\0');
            if (!text.empty())
            {
                const auto read =
                    handle->read(std::span<uint8_t>(reinterpret_cast<uint8_t*>(text.data()), text.size()));
                text.resize(read);
            }
            return std::make_unique<StubTextResource>(id, std::move(text));
        }

    private:
        AssetSource& source_;
    };

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
        manager.registerLoader(TEST_TEXT_TYPE, std::make_unique<StubTextLoader>(a));
        auto first = manager.hold<StubTextResource>(TEST_TEXT_TYPE, "value.txt");
        ASSERT_TRUE(first);
        const auto id = first.id();
        const auto* original = first.get<StubTextResource>();
        EXPECT_EQ(original->text(), "root A");
        EXPECT_THROW(manager.bindAssetSource(&b), Error::Exception);
        manager.reset();
        EXPECT_THROW(manager.bindAssetSource(nullptr), Error::Exception);
        EXPECT_THROW(manager.bindAssetSource(&b), Error::Exception);
        EXPECT_EQ(manager.assetSource(), &a);
        EXPECT_EQ(registry.getObject(id), original);
        first.reset();
        EXPECT_FALSE(registry.hasLiveResources());
        manager.bindAssetSource(nullptr);
        manager.bindAssetSource(&b);
        manager.registerLoader(TEST_TEXT_TYPE, std::make_unique<StubTextLoader>(b));
        auto second = manager.hold<StubTextResource>(TEST_TEXT_TYPE, "value.txt");
        ASSERT_TRUE(second);
        EXPECT_EQ(second.id(), id);
        EXPECT_EQ(second.get<StubTextResource>()->text(), "root B");
        second.reset();
        manager.reset();
        manager.bindAssetSource(nullptr);
    }

    TEST_F(AssetSourceTest, UnownedMountedPayloadAlsoPreventsSourceDetachment)
    {
        AssetSource source(*fileIO_, directory_ / "A");
        auto& manager = ResourceManager::instance();
        auto& registry = ResourceRegistry::instance();
        manager.bindAssetSource(&source);
        manager.registerLoader(TEST_TEXT_TYPE, std::make_unique<StubTextLoader>(source));
        auto hold = manager.hold<StubTextResource>(TEST_TEXT_TYPE, "value.txt");
        ASSERT_TRUE(hold);
        const auto id = hold.id();
        registry.acquire(id);
        hold.reset();
        registry.release(id);  // Registry 允许零持有且挂载，Manager 解除绑定仍须拒绝。
        manager.reset();
        EXPECT_THROW(manager.bindAssetSource(nullptr), Error::Exception);
        EXPECT_EQ(manager.assetSource(), &source);
        registry.detachObject(id);
        manager.bindAssetSource(nullptr);
    }
}  // namespace
