#include "runtime/scene/scene.h"

#include <gtest/gtest.h>

#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    using Incubator::Scene::Camera;
    using Incubator::Scene::Entity;
    using Incubator::Scene::EntityId;
    using Incubator::Scene::Scene;
    using Incubator::Scene::Transform;

    static_assert(!std::is_copy_constructible_v<Scene> && !std::is_copy_assignable_v<Scene>);
    static_assert(!std::is_move_constructible_v<Scene> && !std::is_move_assignable_v<Scene>);
    static_assert(std::is_same_v<decltype(std::declval<Scene&>().find(EntityId{})), const Entity*>);
    static_assert(std::is_same_v<decltype(std::declval<Scene&>().entities()), std::span<const Entity>>);
    static_assert(std::is_same_v<decltype(std::declval<Scene&>().camera()), const Camera&>);

    std::vector<EntityId> ids(const Scene& scene)
    {
        std::vector<EntityId> result;
        for (const auto& entity : scene.entities())
            result.push_back(entity.id);
        return result;
    }

    TEST(SceneEntityCpuTest, DefaultsAndLogicalEntitiesOwnIndependentValues)
    {
        Scene scene;
        EXPECT_TRUE(scene.entities().empty());
        EXPECT_FALSE(EntityId{}.valid());
        EXPECT_EQ(scene.find({}), nullptr);
        EXPECT_EQ(scene.camera().matrices({800, 600}).view, glm::mat4(1));
        const auto first = scene.createEntity();
        Transform input({1, 2, 3}, {1, 0, 0, 1}, {2, -3, 0});
        const auto second = scene.createEntity({}, input, false);
        ASSERT_TRUE(first.valid());
        ASSERT_TRUE(second.valid());
        EXPECT_NE(first, second);
        ASSERT_NE(scene.find(first), nullptr);
        ASSERT_NE(scene.find(second), nullptr);
        EXPECT_EQ(scene.find(first)->id, first);
        EXPECT_EQ(scene.find(first)->transform.matrix(), glm::mat4(1));
        EXPECT_TRUE(scene.find(first)->visible);
        EXPECT_EQ(scene.find(first)->model, nullptr);
        EXPECT_EQ(scene.find(second)->transform.matrix(), input.matrix());
        EXPECT_FALSE(scene.find(second)->visible);
        EXPECT_EQ(scene.find(second)->model, nullptr);
        input.setPosition({9, 9, 9});
        EXPECT_EQ(scene.find(second)->transform.position(), glm::vec3(1, 2, 3));
    }

    TEST(SceneEntityCpuTest, MutationsPreserveCreationOrderAndOtherEntityState)
    {
        Scene scene;
        const auto first = scene.createEntity();
        const auto second = scene.createEntity();
        const auto third = scene.createEntity();
        Transform moved({1, 2, 3}, {1, 0, 0, 1}, {2, 3, 4});
        ASSERT_TRUE(scene.setTransform(second, moved));
        ASSERT_TRUE(scene.setVisible(second, false));
        ASSERT_TRUE(scene.setModel(second, {}));
        EXPECT_EQ(ids(scene), (std::vector{first, second, third}));
        EXPECT_EQ(scene.find(second)->transform.matrix(), moved.matrix());
        EXPECT_FALSE(scene.find(second)->visible);
        for (const auto id : {first, third})
        {
            EXPECT_EQ(scene.find(id)->transform.matrix(), glm::mat4(1));
            EXPECT_TRUE(scene.find(id)->visible);
            EXPECT_EQ(scene.find(id)->model, nullptr);
        }
        ASSERT_TRUE(scene.setVisible(second, true));
        EXPECT_TRUE(scene.find(second)->visible);
    }

    TEST(SceneEntityCpuTest, DeletionClearAndContainerGrowthPreserveIdsWithoutReuse)
    {
        Scene scene;
        const auto first = scene.createEntity();
        const auto middle = scene.createEntity();
        const auto last = scene.createEntity();
        ASSERT_TRUE(scene.destroyEntity(middle));
        EXPECT_EQ(scene.find(middle), nullptr);
        EXPECT_FALSE(scene.destroyEntity(middle));
        const auto added = scene.createEntity();
        EXPECT_NE(added, middle);
        EXPECT_EQ(ids(scene), (std::vector{first, last, added}));
        // vector 扩容后按 ID 重新获取实体，不要保留先前借用的指针或视图。
        for (int count = 0; count < 32; ++count)
            scene.createEntity();
        EXPECT_EQ(scene.find(first)->id, first);
        EXPECT_EQ(scene.find(last)->id, last);
        EXPECT_EQ(scene.find(added)->id, added);
        const auto old = ids(scene);
        scene.clear();
        EXPECT_TRUE(scene.entities().empty());
        const auto fresh = scene.createEntity();
        for (const auto id : old)
        {
            EXPECT_EQ(scene.find(id), nullptr);
            EXPECT_NE(fresh, id);
        }
        EXPECT_NE(fresh, middle);
        EXPECT_EQ(ids(scene), (std::vector{fresh}));
        ASSERT_TRUE(scene.destroyEntity(fresh));
        EXPECT_TRUE(scene.entities().empty());
        scene.clear();
    }

    TEST(SceneEntityCpuTest, MissingDeletedClearedAndForeignIdsCannotModifyAnotherEntity)
    {
        Scene scene;
        Scene other;
        const auto live = scene.createEntity();
        const auto foreign = other.createEntity();
        const auto deleted = scene.createEntity();
        ASSERT_TRUE(scene.destroyEntity(deleted));
        const auto cleared = other.createEntity();
        other.clear();
        const auto otherLive = other.createEntity();
        EXPECT_NE(live, otherLive);
        EXPECT_EQ(other.find(live), nullptr);
        for (const auto id : {EntityId{}, foreign, deleted, cleared, otherLive})
        {
            EXPECT_EQ(scene.find(id), nullptr);
            EXPECT_FALSE(scene.setTransform(id, Transform({9, 9, 9})));
            EXPECT_FALSE(scene.setVisible(id, false));
            EXPECT_FALSE(scene.setModel(id, {}));
            EXPECT_FALSE(scene.destroyEntity(id));
            EXPECT_EQ(ids(scene), (std::vector{live}));
            EXPECT_EQ(scene.find(live)->transform.matrix(), glm::mat4(1));
            EXPECT_TRUE(scene.find(live)->visible);
            EXPECT_EQ(scene.find(live)->model, nullptr);
        }
        EXPECT_FALSE(other.setVisible(live, false));
        EXPECT_FALSE(other.destroyEntity(live));
        EXPECT_TRUE(other.find(otherLive)->visible);
    }

    TEST(SceneEntityCpuTest, SceneDestructionAndInterleavedCreationDoNotReuseIdentities)
    {
        std::vector<EntityId> old;
        {
            Scene first;
            Scene second;
            old = {first.createEntity(), second.createEntity(), first.createEntity(), second.createEntity()};
            for (size_t index = 1; index < old.size(); ++index)
                EXPECT_GT(old[index].value(), old[index - 1].value());
        }
        Scene fresh;
        const auto id = fresh.createEntity();
        EXPECT_GT(id.value(), old.back().value());
        for (const auto previous : old)
        {
            EXPECT_EQ(fresh.find(previous), nullptr);
            EXPECT_FALSE(fresh.destroyEntity(previous));
        }
        EXPECT_EQ(ids(fresh), (std::vector{id}));
    }

    TEST(SceneEntityCpuTest, CameraReplacementOwnsItsValueAndSurvivesClear)
    {
        Scene scene;
        Scene other;
        Camera camera({1, 2, 3}, {1, 0, 1, 0}, {glm::radians(60.0f), 0.5f, 20});
        const auto expected = camera.matrices({800, 600});
        scene.setCamera(camera);
        camera.setPosition({9, 9, 9});
        const auto old = scene.createEntity();
        scene.clear();
        EXPECT_EQ(scene.find(old), nullptr);
        EXPECT_TRUE(scene.entities().empty());
        EXPECT_EQ(scene.camera().position(), glm::vec3(1, 2, 3));
        EXPECT_EQ(scene.camera().matrices({800, 600}).view, expected.view);
        EXPECT_EQ(scene.camera().matrices({800, 600}).projection, expected.projection);
        EXPECT_EQ(other.camera().position(), glm::vec3(0));
        EXPECT_EQ(other.camera().matrices({800, 600}).view, glm::mat4(1));
        scene.createEntity();
        EXPECT_EQ(scene.camera().matrices({800, 600}).view, expected.view);
    }
}  // namespace
