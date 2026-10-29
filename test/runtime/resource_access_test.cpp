#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <type_traits>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/resource/loader/model_loader.h"
#include "runtime/resource/loader/shader_resource.h"
#include "runtime/resource/loader/text_loader.h"
#include "runtime/resource/loader/text_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource;
    namespace fs = std::filesystem;
    static_assert(!std::is_copy_constructible_v<CpuResource>);
    static_assert(std::is_nothrow_move_constructible_v<CpuResource>);

    class ResourceAccessTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            manager().reset();
            ResourceRegistry::instance().clear();
            manager().bindAssetSource(nullptr);
            Log::Registry::reset();
            Log::Registry::get("resource").addSink(std::make_shared<spdlog::sinks::ostream_sink_mt>(messages_));
            root_ = fs::current_path() / "review/resource-access" /
                    ::testing::UnitTest::GetInstance()->current_test_info()->name();
            fs::remove_all(root_);
            fs::create_directories(root_ / "empty");
            io_ = FileIO::createPlatformFileIO();
            source_ = std::make_unique<AssetSource>(*io_, root_);
            manager().bindAssetSource(source_.get());
            manager().registerLoader(ResourceType::TEXT, std::make_unique<TextLoader>(*source_));
            write("value.txt", "payload");
        }
        void TearDown() override
        {
            EXPECT_FALSE(ResourceRegistry::instance().hasLiveResources());
            manager().reset();
            ResourceRegistry::instance().clear();
            manager().bindAssetSource(nullptr);
            source_.reset();
            io_.reset();
            Log::Registry::reset();
        }
        ResourceManager& manager()
        {
            return ResourceManager::instance();
        }
        void write(std::string_view name, std::string_view text)
        {
            std::ofstream(root_ / Utils::Utf8::toFsPath(name)) << text;
        }
        size_t errors() const
        {
            size_t count = 0;
            for (auto at = messages_.str().find("load:"); at != std::string::npos;
                 at = messages_.str().find("load:", at + 5))
                ++count;
            return count;
        }
        fs::path root_;
        std::unique_ptr<FileIO::FileIO> io_;
        std::unique_ptr<AssetSource> source_;
        std::ostringstream messages_;
    };

    TEST_F(ResourceAccessTest, DiscoveryIsSortedClassifiedRefreshableAndDoesNotRegisterOrLoad)
    {
        write("Z.OBJ", "not a model");
        write("材质.mtl", "dependency");
        write("buffer.bin", "dependency");
        write("texture.PNG", "not a PNG");
        write("scene.json", "{}");
        const auto first = manager().listDirectory();
        ASSERT_EQ(first.size(), 7u);
        EXPECT_EQ(first.front().path, "buffer.bin");
        EXPECT_TRUE(std::ranges::is_sorted(first, {}, &AssetEntry::path));
        for (const auto& item : first)
        {
            EXPECT_FALSE(manager().query(ResourceType::MODEL, item.path));
            EXPECT_FALSE(manager().query(ResourceType::TEXT, item.path));
            EXPECT_FALSE(manager().query(ResourceType::TEXTURE, item.path));
            if (item.name == "Z.OBJ")
                EXPECT_EQ(item.kind, AssetKind::Model);
            if (item.name == "texture.PNG")
                EXPECT_EQ(item.kind, AssetKind::Texture);
            if (item.name == "scene.json")
                EXPECT_EQ(item.kind, AssetKind::File);
            if (item.name == "buffer.bin" || item.name == "材质.mtl")
                EXPECT_EQ(item.kind, AssetKind::Dependency);
        }
        EXPECT_FALSE(ResourceRegistry::instance().hasLiveResources());
        EXPECT_TRUE(manager().listDirectory("empty").empty());
        write("new.gltf", "{}");
        EXPECT_EQ(manager().listDirectory().size(), first.size() + 1);
        fs::remove(root_ / "new.gltf");
        EXPECT_EQ(manager().listDirectory().size(), first.size());
        fs::create_directory(root_ / Utils::Utf8::toFsPath("子目录"));
        write("子目录/模型.GLTF", "{}");
        const auto child = manager().listDirectory("子目录");
        ASSERT_EQ(child.size(), 1u);
        EXPECT_EQ(child[0].name, "模型.GLTF");
        EXPECT_EQ(child[0].path, "子目录/模型.gltf");
        EXPECT_EQ(child[0].kind, AssetKind::Model);
        EXPECT_EQ(manager().listDirectory(".").size(), first.size() + 1);
        fs::remove(root_ / "empty");
        EXPECT_THROW(manager().listDirectory("empty"), Error::Exception);
        EXPECT_THROW(manager().listDirectory("value.txt"), Error::Exception);
        EXPECT_THROW(manager().listDirectory("../outside"), Error::Exception);
        EXPECT_THROW(io_->listDirectory(root_ / "missing"), Error::Exception);
        EXPECT_EQ(errors(), 0u);
    }

    TEST_F(ResourceAccessTest, MoveOnlyHoldsBalanceFirstExistingAndUnloadedRegistrations)
    {
        auto first = manager().hold<TextResource>(ResourceType::TEXT, "value.txt");
        ASSERT_TRUE(first);
        const auto id = first.id();
        const auto* data = first.get<TextResource>();
        auto second = manager().hold<TextResource>(ResourceType::TEXT, "value.txt");
        EXPECT_EQ(second.get<TextResource>(), data);
        EXPECT_EQ(manager().query(ResourceType::TEXT, "value.txt")->references, 2u);
        auto moved = std::move(first);
        EXPECT_FALSE(first);
        second = std::move(moved);
        EXPECT_FALSE(moved);
        EXPECT_EQ(second.id(), id);
        EXPECT_EQ(manager().query(ResourceType::TEXT, "value.txt")->references, 1u);
        second.reset();
        EXPECT_EQ(manager().query(ResourceType::TEXT, "value.txt")->references, 0u);
        EXPECT_FALSE(manager().query(ResourceType::TEXT, "value.txt")->loaded);
        auto again = manager().hold<TextResource>(ResourceType::TEXT, "value.txt");
        EXPECT_EQ(again.id(), id);
        EXPECT_EQ(again.get<TextResource>()->text(), "payload");
    }

    TEST_F(ResourceAccessTest, FailureAndTypeMismatchReleaseOnlyTheirOwnReference)
    {
        auto first = manager().hold<TextResource>(ResourceType::TEXT, "value.txt");
        const auto* retained = first.get<TextResource>();
        auto mismatch = manager().hold<ShaderResource>(ResourceType::TEXT, "value.txt");
        EXPECT_FALSE(mismatch);
        ASSERT_TRUE(mismatch.failure());
        EXPECT_NE(mismatch.failure()->reason.find("type mismatch"), std::string::npos);
        EXPECT_EQ(manager().query(ResourceType::TEXT, "value.txt")->references, 1u);
        EXPECT_EQ(first.get<TextResource>(), retained);
        auto missing = manager().hold<TextResource>(ResourceType::TEXT, "missing.txt");
        EXPECT_FALSE(missing);
        ASSERT_TRUE(missing.failure());
        EXPECT_EQ(missing.failure()->path, "missing.txt");
        EXPECT_NE(missing.failure()->reason.find("cannot open"), std::string::npos);
        EXPECT_FALSE(manager().query(ResourceType::TEXT, "missing.txt")->loaded);
        EXPECT_EQ(manager().query(ResourceType::TEXT, "missing.txt")->references, 0u);
        EXPECT_EQ(errors(), 2u);
    }

    TEST_F(ResourceAccessTest, ModelFailuresOwnSpecificReasonsAcrossSubsequentCallsAndRepair)
    {
        manager().registerLoader(ResourceType::MODEL, std::make_unique<ModelLoader>(*source_));
        write("broken.obj", R"(mtllib missing.mtl
v 0 0 0
v 1 0 0
v 0 1 0
f 1 2 3
)");
        auto missing = manager().hold(ResourceType::MODEL, "broken.obj");
        ASSERT_TRUE(missing.failure());
        const auto reason = missing.failure()->reason;
        EXPECT_NE(reason.find("missing.mtl"), std::string::npos);
        write("broken.gltf", "{");
        auto invalid = manager().hold(ResourceType::MODEL, "broken.gltf");
        ASSERT_TRUE(invalid.failure());
        EXPECT_NE(invalid.failure()->reason.find("Invalid glTF JSON"), std::string::npos);
        EXPECT_EQ(missing.failure()->reason, reason);
        EXPECT_EQ(errors(), 2u);
        write("missing.mtl", R"(newmtl material
Kd 1 1 1
)");
        auto repaired = manager().hold(ResourceType::MODEL, "broken.obj");
        EXPECT_TRUE(repaired);
        EXPECT_EQ(errors(), 2u);
    }
}  // namespace
