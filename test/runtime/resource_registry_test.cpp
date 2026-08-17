#define TESTING_ENABLED

#include "runtime/resource/resource_registry.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using namespace Incubator::Resource;

class ResourceRegistryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ResourceRegistry::instance().clear();
    }
};

TEST_F(ResourceRegistryTest, RegisterReturnsStableId)
{
    auto& reg = ResourceRegistry::instance();
    ResourceId id1 = reg.registerResource(ResourceType::TEXTURE, "textures/player.png");
    ResourceId id2 = reg.registerResource(ResourceType::TEXTURE, "textures/player.png");
    EXPECT_TRUE(id1.valid());
    EXPECT_EQ(id1, id2);
}

TEST_F(ResourceRegistryTest, RegisterNormalizesPath)
{
    auto& reg = ResourceRegistry::instance();
    ResourceId id1 = reg.registerResource(ResourceType::TEXTURE, "textures/../textures/Player.png");
    ResourceId id2 = reg.registerResource(ResourceType::TEXTURE, "TEXTURES/player.png");
    EXPECT_EQ(id1, id2);
}

TEST_F(ResourceRegistryTest, DifferentTypesSeparateEntries)
{
    auto& reg = ResourceRegistry::instance();
    ResourceId tex = reg.registerResource(ResourceType::TEXTURE, "ui/icons");
    ResourceId shd = reg.registerResource(ResourceType::SHADER, "ui/icons");
    EXPECT_NE(tex, shd);
}

TEST_F(ResourceRegistryTest, InvalidInputRejected)
{
    auto& reg = ResourceRegistry::instance();
    EXPECT_FALSE(reg.registerResource(ResourceType::INVALID, "textures/player.png").valid());
    EXPECT_FALSE(reg.registerResource(ResourceType::TEXTURE, "").valid());
    EXPECT_FALSE(reg.registerResource(ResourceType::TEXTURE, "C:/abs/path.png").valid());
    EXPECT_FALSE(reg.registerResource(ResourceType::TEXTURE, "//server/share/p.png").valid());
}

TEST_F(ResourceRegistryTest, CollisionRejected)
{
    auto& reg = ResourceRegistry::instance();
    ResourceId idA = ResourceId::fromString("1234567890abcdef");

    EXPECT_TRUE(reg.injectEntry(ResourceType::TEXTURE, "textures/conflict", idA));

    // 不同 key 注入相同 ID → 碰撞被拒绝
    EXPECT_FALSE(reg.injectEntry(ResourceType::TEXTURE, "textures/other", idA));

    // 原条目未被覆盖
    ResourceType type = ResourceType::INVALID;
    std::string path;
    ASSERT_TRUE(reg.lookup(idA, type, path));
    EXPECT_EQ(type, ResourceType::TEXTURE);
    EXPECT_EQ(path, "textures/conflict");
}

TEST_F(ResourceRegistryTest, CollisionSameKeySameIdIsIdempotent)
{
    auto& reg = ResourceRegistry::instance();
    ResourceId idA = ResourceId::fromString("1234567890abcdef");
    EXPECT_TRUE(reg.injectEntry(ResourceType::TEXTURE, "textures/conflict", idA));
    EXPECT_TRUE(reg.injectEntry(ResourceType::TEXTURE, "textures/conflict", idA));
}

TEST_F(ResourceRegistryTest, InjectEntryRejectsPathMappingToDifferentId)
{
    auto& reg = ResourceRegistry::instance();
    ResourceId id = reg.registerResource(ResourceType::TEXTURE, "textures/player.png");

    // (type, key) 已映射到不同 ID → 拒绝
    EXPECT_FALSE(reg.injectEntry(ResourceType::TEXTURE, "textures/player.png",
                                 ResourceId::fromString("1234567890abcdef")));

    ResourceType type = ResourceType::INVALID;
    std::string path;
    ASSERT_TRUE(reg.lookup(id, type, path));
    EXPECT_EQ(type, ResourceType::TEXTURE);
    EXPECT_EQ(path, "textures/player.png");
}

TEST_F(ResourceRegistryTest, RefCountAcquireRelease)
{
    auto& reg = ResourceRegistry::instance();
    ResourceId id = reg.registerResource(ResourceType::MODEL, "models/car.fbx");
    EXPECT_EQ(reg.refCount(id), 1u);

    reg.acquire(id);
    reg.acquire(id);
    EXPECT_EQ(reg.refCount(id), 3u);

    reg.release(id);
    EXPECT_EQ(reg.refCount(id), 2u);
}

TEST_F(ResourceRegistryTest, RefCountUnknownIdIsZero)
{
    auto& reg = ResourceRegistry::instance();
    EXPECT_EQ(reg.refCount(ResourceId::fromString("ffffffffffffffff")), 0u);
}

TEST_F(ResourceRegistryTest, ReleaseDoesNotGoNegative)
{
    auto& reg = ResourceRegistry::instance();
    ResourceId id = reg.registerResource(ResourceType::MODEL, "models/car.fbx");
    reg.release(id);
    reg.release(id);
    EXPECT_EQ(reg.refCount(id), 0u);
}

TEST_F(ResourceRegistryTest, LookupById)
{
    auto& reg = ResourceRegistry::instance();
    ResourceId id = reg.registerResource(ResourceType::SHADER, "shaders/pbr.vert");

    ResourceType type = ResourceType::INVALID;
    std::string path;
    ASSERT_TRUE(reg.lookup(id, type, path));
    EXPECT_EQ(type, ResourceType::SHADER);
    EXPECT_EQ(path, "shaders/pbr.vert");

    EXPECT_FALSE(reg.lookup(ResourceId::fromString("deadbeefdeadbeef"), type, path));
}

TEST_F(ResourceRegistryTest, LookupByTypeAndPath)
{
    auto& reg = ResourceRegistry::instance();
    ResourceId id = reg.registerResource(ResourceType::MATERIAL, "materials/gold.mat");

    EXPECT_EQ(reg.lookup(ResourceType::MATERIAL, "materials/gold.mat"), id);
    EXPECT_FALSE(reg.lookup(ResourceType::MATERIAL, "materials/silver.mat").valid());
    EXPECT_FALSE(reg.lookup(ResourceType::TEXTURE, "materials/gold.mat").valid());
}

TEST_F(ResourceRegistryTest, ClearResetsState)
{
    auto& reg = ResourceRegistry::instance();
    ResourceId id = reg.registerResource(ResourceType::MATERIAL, "materials/gold.mat");
    reg.clear();
    EXPECT_EQ(reg.refCount(id), 0u);
    EXPECT_FALSE(reg.lookup(ResourceType::MATERIAL, "materials/gold.mat").valid());
}