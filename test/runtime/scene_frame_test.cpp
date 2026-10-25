#include "runtime/scene/scene_frame.h"

#include <gtest/gtest.h>

#include <utility>

#include "runtime/scene/scene.h"

namespace
{
    using namespace Incubator;

    template <class T>
    concept CanBorrowInput = requires(T&& frame) { std::forward<T>(frame).input(); };
    static_assert(CanBorrowInput<Scene::SceneFrame&>);
    static_assert(CanBorrowInput<const Scene::SceneFrame&>);
    static_assert(!CanBorrowInput<Scene::SceneFrame>);

    TEST(SceneFrameCpuTest, ZeroExtentSkipsExtraction)
    {
        Scene::Scene scene;
        scene.createEntity();
        for (const Render::FrameExtent extent : {Render::FrameExtent{0, 0}, {0, 600}, {800, 0}})
            EXPECT_FALSE(Scene::extractSceneFrame(scene, extent));
        EXPECT_TRUE(Scene::extractSceneFrame(scene, {800, 600}));
    }

    TEST(SceneFrameCpuTest, EmptyAndLogicalEntitiesProduceFreshEmptyLists)
    {
        Scene::Scene scene;
        const auto empty = Scene::extractSceneFrame(scene, {800, 600});
        ASSERT_TRUE(empty);
        EXPECT_TRUE(empty->draws.empty());
        const auto visible = scene.createEntity();
        scene.createEntity({}, Scene::Transform{}, false);
        auto logical = Scene::extractSceneFrame(scene, {800, 600});
        ASSERT_TRUE(logical);
        EXPECT_TRUE(logical->draws.empty());
        EXPECT_EQ(logical->input().extent, (Render::FrameExtent{800, 600}));
        EXPECT_EQ(logical->input().draws.data(), logical->draws.data());
        EXPECT_TRUE(scene.setVisible(visible, false));
        scene.clear();
        EXPECT_TRUE(Scene::extractSceneFrame(scene, {800, 600})->draws.empty());
        EXPECT_TRUE(empty->draws.empty());
        EXPECT_TRUE(logical->draws.empty());
    }

    TEST(SceneFrameCpuTest, CameraLightingAndExtentAreSnapshotValuesIndependentOfSceneLifetime)
    {
        const Render::FrameInput legacy{{800, 400}, {glm::mat4(1), glm::mat4(1)}, {}};
        const Scene::SceneFrame legacySnapshot{{800, 400}, {glm::mat4(1), glm::mat4(1)}, {}};
        EXPECT_EQ(legacy.lighting, Render::Lighting{});
        EXPECT_EQ(legacySnapshot.input().lighting, Render::Lighting{});
        const Render::Lighting lighting{{3, 0, -4}, {1, 0.5f, 0.25f}, 2, 0.2f};
        Scene::SceneFrame retained;
        {
            Scene::Scene scene;
            scene.setLighting(lighting);
            scene.setCamera(Scene::Camera({0, 0, 3}, {1, 0, 0, 0}, {glm::radians(90.0f), 0.1f, 10}));
            auto first = Scene::extractSceneFrame(scene, {800, 400});
            ASSERT_TRUE(first);
            EXPECT_NEAR(first->camera.projection[0][0], 0.5f, 0.00001f);
            EXPECT_FLOAT_EQ(first->camera.view[3][2], -3);
            scene.setLighting({{0, -2, 0}, {0.5f, 1, 0.25f}, 0, 0.7f});
            scene.setCamera(Scene::Camera({0, 0, 5}, {1, 0, 0, 0}, {glm::radians(90.0f), 1, 20}));
            const auto next = Scene::extractSceneFrame(scene, {400, 400});
            ASSERT_TRUE(next);
            EXPECT_EQ(first->lighting, lighting);
            EXPECT_EQ(first->input().lighting, lighting);
            EXPECT_EQ(next->lighting, scene.lighting());
            EXPECT_NE(first->lighting, next->lighting);
            EXPECT_NEAR(next->camera.projection[0][0], 1, 0.00001f);
            EXPECT_FLOAT_EQ(next->camera.view[3][2], -5);
            EXPECT_NE(first->camera.projection, next->camera.projection);
            EXPECT_NE(first->camera.view, next->camera.view);
            EXPECT_EQ(first->extent, (Render::FrameExtent{800, 400}));
            retained = *first;
            auto moved = std::move(*first);
            EXPECT_EQ(retained.input().camera.view, moved.input().camera.view);
            EXPECT_EQ(retained.input().camera.projection, moved.input().camera.projection);
            EXPECT_EQ(retained.input().lighting, lighting);
            EXPECT_EQ(moved.input().lighting, lighting);
            const auto borrowed = moved.input();
            moved.lighting.ambient = 0.9f;
            EXPECT_EQ(borrowed.lighting, lighting);
            EXPECT_EQ(retained.lighting, lighting);
        }
        EXPECT_EQ(retained.input().extent, (Render::FrameExtent{800, 400}));
        EXPECT_FLOAT_EQ(retained.input().camera.view[3][2], -3);
        EXPECT_TRUE(retained.input().draws.empty());
        EXPECT_EQ(retained.input().lighting, lighting);
    }
}  // namespace
