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

    class ThrowingLoader final : public LoaderBase
    {
    public:
        std::unique_ptr<ResourceObject> load(const ResourceId&, std::string_view) override
        {
            throw Error::Exception(Error::Code::IO, "simulated loader io failure");
        }
    };

    class NullLoader final : public LoaderBase
    {
    public:
        std::unique_ptr<ResourceObject> load(const ResourceId&, std::string_view) override
        {
            return nullptr;
        }
    };

    class StubTextLoader final : public LoaderBase
    {
    public:
        explicit StubTextLoader(AssetSource& source) : source_(source) {}

        std::unique_ptr<ResourceObject> load(const ResourceId& id, std::string_view virtualPath) override
        {
            return loadResult(id, virtualPath).object;
        }

        LoaderResult loadResult(const ResourceId& id, std::string_view virtualPath) override
        {
            auto handle = source_.open(virtualPath);
            if (!handle)
                return {nullptr, fmt::format("StubTextLoader: cannot open '{}'", virtualPath), false};
            std::string text(static_cast<size_t>(handle->size()), '\0');
            if (!text.empty())
            {
                const auto read =
                    handle->read(std::span<uint8_t>(reinterpret_cast<uint8_t*>(text.data()), text.size()));
                text.resize(read);
            }
            return {std::make_unique<StubTextResource>(id, std::move(text)), {}, false};
        }

    private:
        AssetSource& source_;
    };

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
            manager().registerLoader(TEST_TEXT_TYPE, std::make_unique<StubTextLoader>(*source_));
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
            EXPECT_FALSE(manager().query(TEST_TEXT_TYPE, item.path));
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
        auto first = manager().hold<StubTextResource>(TEST_TEXT_TYPE, "value.txt");
        ASSERT_TRUE(first);
        const auto id = first.id();
        const auto* data = first.get<StubTextResource>();
        auto second = manager().hold<StubTextResource>(TEST_TEXT_TYPE, "value.txt");
        EXPECT_EQ(second.get<StubTextResource>(), data);
        EXPECT_EQ(manager().query(TEST_TEXT_TYPE, "value.txt")->references, 2u);
        auto moved = std::move(first);
        EXPECT_FALSE(first);
        second = std::move(moved);
        EXPECT_FALSE(moved);
        EXPECT_EQ(second.id(), id);
        EXPECT_EQ(manager().query(TEST_TEXT_TYPE, "value.txt")->references, 1u);
        second.reset();
        EXPECT_EQ(manager().query(TEST_TEXT_TYPE, "value.txt")->references, 0u);
        EXPECT_FALSE(manager().query(TEST_TEXT_TYPE, "value.txt")->loaded);
        auto again = manager().hold<StubTextResource>(TEST_TEXT_TYPE, "value.txt");
        EXPECT_EQ(again.id(), id);
        EXPECT_EQ(again.get<StubTextResource>()->text(), "payload");
    }

    TEST_F(ResourceAccessTest, FailureAndTypeMismatchReleaseOnlyTheirOwnReference)
    {
        auto first = manager().hold<StubTextResource>(TEST_TEXT_TYPE, "value.txt");
        const auto* retained = first.get<StubTextResource>();
        auto mismatch = manager().hold<ShaderResource>(TEST_TEXT_TYPE, "value.txt");
        EXPECT_FALSE(mismatch);
        ASSERT_TRUE(mismatch.failure());
        EXPECT_NE(mismatch.failure()->reason.find("type mismatch"), std::string::npos);
        EXPECT_EQ(manager().query(TEST_TEXT_TYPE, "value.txt")->references, 1u);
        EXPECT_EQ(first.get<StubTextResource>(), retained);
        auto missing = manager().hold<StubTextResource>(TEST_TEXT_TYPE, "missing.txt");
        EXPECT_FALSE(missing);
        ASSERT_TRUE(missing.failure());
        EXPECT_EQ(missing.failure()->path, "missing.txt");
        EXPECT_NE(missing.failure()->reason.find("cannot open"), std::string::npos);
        EXPECT_FALSE(manager().query(TEST_TEXT_TYPE, "missing.txt")->loaded);
        EXPECT_EQ(manager().query(TEST_TEXT_TYPE, "missing.txt")->references, 0u);
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

    TEST_F(ResourceAccessTest, LoaderRegistrationOverwriteResetAndFailureDiagnostics)
    {
        manager().reset();
        EXPECT_TRUE(manager().loadersEmpty());
        EXPECT_FALSE(manager().hasLoader(ResourceType::TEXTURE));

        // 无加载器时返回空并按路径和类型记录日志与 0 引用登记
        auto noLoader = manager().hold<StubTextResource>(ResourceType::MODEL, "models/car.fbx");
        EXPECT_FALSE(noLoader);
        ASSERT_TRUE(noLoader.failure());
        EXPECT_NE(messages_.str().find("models/car.fbx"), std::string::npos);
        EXPECT_NE(messages_.str().find(fmt::format("type {:#x}", uint64_t(ResourceType::MODEL))), std::string::npos);
        const auto missingInfo = manager().query(ResourceType::MODEL, "models/car.fbx");
        ASSERT_TRUE(missingInfo);
        EXPECT_EQ(missingInfo->references, 0u);
        EXPECT_FALSE(missingInfo->loaded);

        // 注册、覆盖与重置加载器
        manager().registerLoader(ResourceType::TEXTURE, std::make_unique<NullLoader>());
        EXPECT_TRUE(manager().hasLoader(ResourceType::TEXTURE));
        EXPECT_FALSE(manager().loadersEmpty());
        auto nullHold = manager().hold<StubTextResource>(ResourceType::TEXTURE, "textures/missing.png");
        EXPECT_FALSE(nullHold);
        const auto nullId = ResourceRegistry::instance().lookup(ResourceType::TEXTURE, "textures/missing.png");
        ASSERT_TRUE(nullId.valid());
        EXPECT_EQ(ResourceRegistry::instance().getObject(nullId), nullptr);

        // Loader 抛异常由管理器吞并并携带 type/id 上下文记录日志
        manager().registerLoader(ResourceType::TEXTURE, std::make_unique<ThrowingLoader>());
        auto boomHold = manager().hold<StubTextResource>(ResourceType::TEXTURE, "textures/boom.png");
        EXPECT_FALSE(boomHold);
        ASSERT_TRUE(boomHold.failure());
        const auto boomId = ResourceRegistry::instance().lookup(ResourceType::TEXTURE, "textures/boom.png");
        ASSERT_TRUE(boomId.valid());
        EXPECT_EQ(ResourceRegistry::instance().getObject(boomId), nullptr);
        EXPECT_NE(messages_.str().find("textures/boom.png"), std::string::npos);
        EXPECT_NE(messages_.str().find("simulated loader io failure"), std::string::npos);
        EXPECT_NE(messages_.str().find(fmt::format("type {:#x}, id {:#x}", uint64_t(ResourceType::TEXTURE),
                                                   boomId.value())),
                  std::string::npos);

        // reset 清空加载器；重新覆盖加载器时仅新加载器生效
        manager().reset();
        EXPECT_TRUE(manager().loadersEmpty());
        EXPECT_FALSE(manager().hasLoader(ResourceType::TEXTURE));
        manager().registerLoader(TEST_TEXT_TYPE, std::make_unique<NullLoader>());
        manager().registerLoader(TEST_TEXT_TYPE, std::make_unique<StubTextLoader>(*source_));
        auto reloaded = manager().hold<StubTextResource>(TEST_TEXT_TYPE, "value.txt");
        ASSERT_TRUE(reloaded);
        EXPECT_EQ(reloaded.get<StubTextResource>()->text(), "payload");
    }
}  // namespace
