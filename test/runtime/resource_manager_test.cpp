#define TESTING_ENABLED

#include "runtime/resource/resource_manager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "runtime/core/error/error.h"
#include "runtime/resource/resource_registry.h"

using namespace Incubator::Resource;

namespace
{
    class FakeResource final : public ResourceObject
    {
    public:
        FakeResource(ResourceId id, ResourceType type) : ResourceObject(id, type) {}
    };

    class OtherResource final : public ResourceObject
    {
    public:
        OtherResource(ResourceId id, ResourceType type) : ResourceObject(id, type) {}
    };

    class FakeLoader final : public LoaderBase
    {
    public:
        FakeLoader(ResourceType objectType, int* callCount = nullptr)
            : objectType_(objectType), callCount_(callCount)
        {
        }

        std::unique_ptr<ResourceObject> load(const ResourceId& id,
                                             std::string_view /*virtualPath*/) override
        {
            if (callCount_)
            {
                ++(*callCount_);
            }
            return std::make_unique<FakeResource>(id, objectType_);
        }

    private:
        ResourceType objectType_;
        int* callCount_;
    };

    class ThrowingLoader final : public LoaderBase
    {
    public:
        std::unique_ptr<ResourceObject> load(const ResourceId&,
                                             std::string_view /*virtualPath*/) override
        {
            throw Incubator::Error::Exception(Incubator::Error::Code::IO,
                                              "simulated loader io failure");
        }
    };

    class NullLoader final : public LoaderBase
    {
    public:
        std::unique_ptr<ResourceObject> load(const ResourceId&,
                                             std::string_view /*virtualPath*/) override
        {
            return nullptr;
        }
    };
}  // namespace

class ResourceManagerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ResourceManager::instance().reset();
        ResourceRegistry::instance().clear();
    }
};

TEST_F(ResourceManagerTest, RegisterLoaderAndHasLoader)
{
    auto& manager = ResourceManager::instance();
    EXPECT_TRUE(manager.loadersEmpty());
    EXPECT_FALSE(manager.hasLoader(ResourceType::TEXTURE));

    manager.registerLoader(ResourceType::TEXTURE, std::make_unique<FakeLoader>(ResourceType::TEXTURE));

    EXPECT_TRUE(manager.hasLoader(ResourceType::TEXTURE));
    EXPECT_FALSE(manager.loadersEmpty());
    EXPECT_FALSE(manager.hasLoader(ResourceType::MODEL));
}

TEST_F(ResourceManagerTest, ResetClearsLoaders)
{
    auto& manager = ResourceManager::instance();
    manager.registerLoader(ResourceType::TEXTURE, std::make_unique<FakeLoader>(ResourceType::TEXTURE));
    ASSERT_TRUE(manager.hasLoader(ResourceType::TEXTURE));

    manager.reset();

    EXPECT_TRUE(manager.loadersEmpty());
    EXPECT_FALSE(manager.hasLoader(ResourceType::TEXTURE));
}

TEST_F(ResourceManagerTest, RegisterDelegatesToRegistry)
{
    auto& manager = ResourceManager::instance();
    ResourceId id = manager.registerResource(ResourceType::TEXTURE, "textures/player.png");
    EXPECT_TRUE(id.valid());

    ResourceType type = ResourceType::INVALID;
    std::string path;
    EXPECT_TRUE(ResourceRegistry::instance().lookup(id, type, path));
    EXPECT_EQ(type, ResourceType::TEXTURE);
    EXPECT_EQ(path, "textures/player.png");
}

TEST_F(ResourceManagerTest, LoadUnknownIdReturnsNull)
{
    auto& manager = ResourceManager::instance();
    ResourceId id = ResourceId::fromPath(ResourceType::TEXTURE, "textures/not_registered.png");

    EXPECT_EQ(manager.load(id), nullptr);
}

TEST_F(ResourceManagerTest, LoadRegisteredWithoutLoaderReturnsNull)
{
    auto& manager = ResourceManager::instance();
    ResourceId id = manager.registerResource(ResourceType::MODEL, "models/car.fbx");
    ASSERT_TRUE(id.valid());

    EXPECT_EQ(manager.load(id), nullptr);
}

TEST_F(ResourceManagerTest, LoadSuccessReturnsReadyObject)
{
    auto& manager = ResourceManager::instance();
    manager.registerLoader(ResourceType::TEXTURE, std::make_unique<FakeLoader>(ResourceType::TEXTURE));

    ResourceId id = manager.registerResource(ResourceType::TEXTURE, "textures/player.png");
    ASSERT_TRUE(id.valid());

    ResourceObject* obj = manager.load(id);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->id(), id);
    EXPECT_EQ(obj->type(), ResourceType::TEXTURE);
    EXPECT_EQ(obj->state(), ResourceState::READY);
}

TEST_F(ResourceManagerTest, GetReturnsTypedObject)
{
    auto& manager = ResourceManager::instance();
    manager.registerLoader(ResourceType::TEXTURE, std::make_unique<FakeLoader>(ResourceType::TEXTURE));

    ResourceId id = manager.registerResource(ResourceType::TEXTURE, "textures/player.png");
    ASSERT_TRUE(id.valid());

    FakeResource* typed = manager.get<FakeResource>(id);
    ASSERT_NE(typed, nullptr);
    EXPECT_EQ(typed, manager.load(id));
}

TEST_F(ResourceManagerTest, GetWithWrongTypeReturnsNull)
{
    auto& manager = ResourceManager::instance();
    manager.registerLoader(ResourceType::TEXTURE, std::make_unique<FakeLoader>(ResourceType::TEXTURE));

    ResourceId id = manager.registerResource(ResourceType::TEXTURE, "textures/player.png");
    ASSERT_TRUE(id.valid());

    EXPECT_EQ(manager.get<OtherResource>(id), nullptr);
}

TEST_F(ResourceManagerTest, SecondLoadReusesMountedObject)
{
    auto& manager = ResourceManager::instance();
    int calls = 0;
    manager.registerLoader(ResourceType::TEXTURE,
                           std::make_unique<FakeLoader>(ResourceType::TEXTURE, &calls));

    ResourceId id = manager.registerResource(ResourceType::TEXTURE, "textures/player.png");
    ASSERT_TRUE(id.valid());

    ResourceObject* first = manager.load(id);
    ASSERT_NE(first, nullptr);

    ResourceObject* second = manager.load(id);
    EXPECT_EQ(second, first);
    EXPECT_EQ(calls, 1);
}

TEST_F(ResourceManagerTest, ReleaseToZeroDetachesButRetainsRegistration)
{
    auto& manager = ResourceManager::instance();
    manager.registerLoader(ResourceType::MODEL, std::make_unique<FakeLoader>(ResourceType::MODEL));

    ResourceId id = manager.registerResource(ResourceType::MODEL, "models/car.fbx");
    ASSERT_TRUE(id.valid());
    ASSERT_NE(manager.load(id), nullptr);

    manager.release(id);

    EXPECT_EQ(ResourceRegistry::instance().refCount(id), 0u);
    EXPECT_EQ(ResourceRegistry::instance().getObject(id), nullptr);

    ResourceType type = ResourceType::INVALID;
    std::string path;
    ASSERT_TRUE(ResourceRegistry::instance().lookup(id, type, path));
    EXPECT_EQ(type, ResourceType::MODEL);
}

TEST_F(ResourceManagerTest, RegisterIdempotentDoesNotAddRef)
{
    auto& manager = ResourceManager::instance();
    ResourceId id1 = manager.registerResource(ResourceType::TEXTURE, "textures/player.png");
    ResourceId id2 = manager.registerResource(ResourceType::TEXTURE, "textures/player.png");

    EXPECT_EQ(id1, id2);
    EXPECT_EQ(ResourceRegistry::instance().refCount(id1), 1u);
}

TEST_F(ResourceManagerTest, SecondHolderKeepsObjectAliveUntilOwnRelease)
{
    auto& manager = ResourceManager::instance();
    manager.registerLoader(ResourceType::TEXTURE, std::make_unique<FakeLoader>(ResourceType::TEXTURE));

    // A：登记即持有 ref = 1
    ResourceId id = manager.registerResource(ResourceType::TEXTURE, "textures/shared.png");
    ASSERT_TRUE(id.valid());
    ResourceObject* objA = manager.load(id);
    ASSERT_NE(objA, nullptr);

    // B：显式 acquire → 两处持有
    manager.acquire(id);
    EXPECT_EQ(ResourceRegistry::instance().refCount(id), 2u);

    // A 释放其登记持有 → 计数 1，对象仍然存活且 Ready
    manager.release(id);
    EXPECT_EQ(ResourceRegistry::instance().refCount(id), 1u);
    EXPECT_EQ(manager.load(id), objA);
    EXPECT_EQ(objA->state(), ResourceState::READY);

    // B 释放 → 计数 0 → 卸载但登记保留
    manager.release(id);
    EXPECT_EQ(ResourceRegistry::instance().refCount(id), 0u);
    EXPECT_EQ(ResourceRegistry::instance().getObject(id), nullptr);

    ResourceType type = ResourceType::INVALID;
    std::string path;
    ASSERT_TRUE(ResourceRegistry::instance().lookup(id, type, path));
    EXPECT_EQ(type, ResourceType::TEXTURE);
    EXPECT_EQ(path, "textures/shared.png");
}

TEST_F(ResourceManagerTest, AcquireUnknownIdIsNoOp)
{
    auto& manager = ResourceManager::instance();
    ResourceId unknown = ResourceId::fromString("ffffffffffffffff");

    manager.acquire(unknown);

    EXPECT_EQ(ResourceRegistry::instance().refCount(unknown), 0u);
}

TEST_F(ResourceManagerTest, LoadSwallowsLoaderException)
{
    auto& manager = ResourceManager::instance();
    manager.registerLoader(ResourceType::TEXTURE, std::make_unique<ThrowingLoader>());

    ResourceId id = manager.registerResource(ResourceType::TEXTURE, "textures/boom.png");
    ASSERT_TRUE(id.valid());

    EXPECT_EQ(manager.load(id), nullptr);
    EXPECT_EQ(ResourceRegistry::instance().getObject(id), nullptr);
}

TEST_F(ResourceManagerTest, RegisterLoaderOverwrite)
{
    auto& manager = ResourceManager::instance();
    int firstCalls = 0;
    int secondCalls = 0;
    manager.registerLoader(ResourceType::TEXTURE,
                           std::make_unique<FakeLoader>(ResourceType::TEXTURE, &firstCalls));
    manager.registerLoader(ResourceType::TEXTURE,
                           std::make_unique<FakeLoader>(ResourceType::TEXTURE, &secondCalls));

    ResourceId id = manager.registerResource(ResourceType::TEXTURE, "textures/player.png");
    ASSERT_TRUE(id.valid());

    ASSERT_NE(manager.load(id), nullptr);
    EXPECT_EQ(firstCalls, 0);
    EXPECT_EQ(secondCalls, 1);
}

TEST_F(ResourceManagerTest, LoadLoaderReturningNullLeavesNoObject)
{
    auto& manager = ResourceManager::instance();
    manager.registerLoader(ResourceType::TEXTURE, std::make_unique<NullLoader>());

    ResourceId id = manager.registerResource(ResourceType::TEXTURE, "textures/missing.png");
    ASSERT_TRUE(id.valid());

    EXPECT_EQ(manager.load(id), nullptr);
    EXPECT_EQ(ResourceRegistry::instance().getObject(id), nullptr);
}