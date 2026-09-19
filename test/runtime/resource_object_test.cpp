#include "runtime/resource/asset_info.h"
#include "runtime/resource/resource_object.h"

#include <gtest/gtest.h>

#include <memory>
#include <type_traits>

using namespace Incubator::Resource;

namespace
{
    class TestResource final : public ResourceObject
    {
    public:
        TestResource(ResourceId id, ResourceType type) : ResourceObject(id, type) {}
    };

    class DestructionTrackedResource final : public ResourceObject
    {
    public:
        explicit DestructionTrackedResource(bool& destroyed)
            : ResourceObject(ResourceId::fromPath(ResourceType::TEXTURE, "textures/test.png"),
                             ResourceType::TEXTURE),
              destroyed_(destroyed)
        {
        }

        ~DestructionTrackedResource() override
        {
            destroyed_ = true;
        }

    private:
        bool& destroyed_;
    };

    static_assert(!std::is_copy_constructible_v<ResourceObject>);
    static_assert(!std::is_copy_assignable_v<ResourceObject>);
}  // namespace

TEST(ResourceObjectTest, ExposesConstructionIdentity)
{
    const ResourceId id = ResourceId::fromPath(ResourceType::TEXTURE, "textures/player.png");
    const TestResource resource(id, ResourceType::TEXTURE);

    EXPECT_EQ(resource.id(), id);
    EXPECT_EQ(resource.type(), ResourceType::TEXTURE);
}

TEST(ResourceObjectTest, DefaultsToUnloadedAndChangesState)
{
    TestResource resource(ResourceId::fromPath(ResourceType::MODEL, "models/car.fbx"), ResourceType::MODEL);

    EXPECT_EQ(resource.state(), ResourceState::UNLOADED);
    resource.setState(ResourceState::LOADING);
    EXPECT_EQ(resource.state(), ResourceState::LOADING);
    resource.setState(ResourceState::READY);
    EXPECT_EQ(resource.state(), ResourceState::READY);
}

TEST(ResourceObjectTest, BasePointerDestroysDerivedObject)
{
    bool destroyed = false;
    {
        std::unique_ptr<ResourceObject> resource = std::make_unique<DestructionTrackedResource>(destroyed);
    }

    EXPECT_TRUE(destroyed);
}

TEST(AssetInfoTest, ValidityRequiresOnlyAnIdentity)
{
    AssetInfo info;
    EXPECT_FALSE(info.valid());

    info.id = ResourceId::fromPath(ResourceType::TEXTURE, "textures/player.png");
    EXPECT_TRUE(info.valid());
    EXPECT_EQ(info.type, ResourceType::INVALID);
    EXPECT_TRUE(info.virtualPath.empty());
    EXPECT_EQ(info.contentHash, 0u);
}
