#define TESTING_ENABLED

#include "runtime/resource/resource_id.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using namespace Incubator::Resource;

class ResourceIdTest : public ::testing::Test
{
};

TEST_F(ResourceIdTest, DefaultIsInvalid)
{
    ResourceId id;
    EXPECT_FALSE(id.valid());
    EXPECT_EQ(id.value(), 0ull);
}

TEST_F(ResourceIdTest, SamePathSameId)
{
    EXPECT_EQ(ResourceId::fromPath(BuiltinType::TEXTURE, "textures/player.png"),
              ResourceId::fromPath(BuiltinType::TEXTURE, "textures/player.png"));
}

TEST_F(ResourceIdTest, DifferentTypesDifferentIds)
{
    EXPECT_NE(ResourceId::fromPath(BuiltinType::TEXTURE, "ui/icons"),
              ResourceId::fromPath(BuiltinType::SHADER, "ui/icons"));
}

TEST_F(ResourceIdTest, CaseInsensitive)
{
    EXPECT_EQ(ResourceId::fromPath(BuiltinType::TEXTURE, "textures/Player.png"),
              ResourceId::fromPath(BuiltinType::TEXTURE, "TEXTURES/player.PNG"));
}

TEST_F(ResourceIdTest, SeparatorUnified)
{
    EXPECT_EQ(ResourceId::fromPath(BuiltinType::TEXTURE, "textures\\player.png"),
              ResourceId::fromPath(BuiltinType::TEXTURE, "textures/player.png"));
}

TEST_F(ResourceIdTest, DotDotNormalized)
{
    EXPECT_EQ(ResourceId::fromPath(BuiltinType::TEXTURE, "textures/../textures/player.png"),
              ResourceId::fromPath(BuiltinType::TEXTURE, "textures/player.png"));
}

TEST_F(ResourceIdTest, LeadingSlashStripped)
{
    EXPECT_EQ(ResourceId::fromPath(BuiltinType::TEXTURE, "/textures/player.png"),
              ResourceId::fromPath(BuiltinType::TEXTURE, "textures/player.png"));
}

TEST_F(ResourceIdTest, EmptyPathInvalid)
{
    EXPECT_FALSE(ResourceId::fromPath(BuiltinType::TEXTURE, "").valid());
    EXPECT_FALSE(ResourceId::fromPathOnly("").valid());
}

TEST_F(ResourceIdTest, InvalidTypeTagInvalid)
{
    EXPECT_FALSE(ResourceId::fromPath(BuiltinType::INVALID, "textures/player.png").valid());
}

TEST_F(ResourceIdTest, AbsolutePathsInvalid)
{
    EXPECT_FALSE(ResourceId::fromPath(BuiltinType::TEXTURE, "C:/textures/player.png").valid());
    EXPECT_FALSE(ResourceId::fromPath(BuiltinType::TEXTURE, "//server/share/player.png").valid());
    EXPECT_FALSE(ResourceId::fromPath(BuiltinType::TEXTURE, "\\\\server\\share\\player.png").valid());
}

TEST_F(ResourceIdTest, FromPathOnlyValidAndDifferentFromWithType)
{
    ResourceId only = ResourceId::fromPathOnly("textures/player.png");
    ResourceId typed = ResourceId::fromPath(BuiltinType::TEXTURE, "textures/player.png");
    EXPECT_TRUE(only.valid());
    EXPECT_NE(only, typed);
}

TEST_F(ResourceIdTest, ToStringFromStringRoundTrip)
{
    ResourceId id = ResourceId::fromPath(BuiltinType::MODEL, "models/car.fbx");
    std::string hex = id.toString();
    ResourceId restored = ResourceId::fromString(hex);
    EXPECT_EQ(restored, id);
    EXPECT_TRUE(restored.valid());
}

TEST_F(ResourceIdTest, FromStringInvalid)
{
    EXPECT_FALSE(ResourceId::fromString("").valid());
    EXPECT_FALSE(ResourceId::fromString("zz").valid());
    EXPECT_FALSE(ResourceId::fromString("0").valid());
}