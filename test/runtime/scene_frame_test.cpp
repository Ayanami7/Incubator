#include "runtime/scene/scene_frame.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include "runtime/core/error/error.h"
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
        EXPECT_FALSE(empty->shadow);
        EXPECT_TRUE(empty->shadowDraws.empty());
        EXPECT_TRUE(empty->input().shadowDraws.empty());
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

    TEST(SceneFrameCpuTest, ShadowProjectionMapsFixedWorldBoundsToDepthAndTextureCoordinates)
    {
        Render::ShadowSettings settings;
        settings.enabled = true;
        settings.center = {3, -2, 1};
        settings.halfExtent = {2, 3};
        settings.nearPlane = 1;
        settings.farPlane = 9;
        settings.depthBias = 0.002f;
        const auto shadow = Render::makeShadowFrame(settings, {0, 0, -4});
        ASSERT_TRUE(shadow);
        EXPECT_EQ(shadow->depthBias, settings.depthBias);
        const auto check = [&](glm::vec3 world, glm::vec3 expected) {
            const auto clip = shadow->viewProjection * glm::vec4(world, 1);
            EXPECT_FLOAT_EQ(clip.w, 1);
            const auto ndc = glm::vec3(clip) / clip.w;
            for (int axis = 0; axis < 3; ++axis)
                EXPECT_NEAR(ndc[axis], expected[axis], 0.00001f);
            const auto uv = glm::vec2(ndc) * 0.5f + 0.5f;
            EXPECT_NEAR(uv.x, expected.x * 0.5f + 0.5f, 0.00001f);
            EXPECT_NEAR(uv.y, expected.y * 0.5f + 0.5f, 0.00001f);
        };
        check(settings.center, {0, 0, 0.5f});
        check(settings.center + glm::vec3(2, 3, 4), {1, -1, 0});
        check(settings.center + glm::vec3(-2, -3, -4), {-1, 1, 1});
        check(settings.center + glm::vec3(4, 0, 0), {2, 0, 0.5f});
    }

    TEST(SceneFrameCpuTest, ShadowAxesAndScaledDirectionsProduceFiniteConsistentProjections)
    {
        Render::ShadowSettings settings;
        settings.enabled = true;
        const float maximum = std::numeric_limits<float>::max();
        for (const auto direction :
             std::array{glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, -1, 0),
                        glm::vec3(0, 0, 1), glm::vec3(0, 0, -1), glm::vec3(0.00001f, 1, 0), glm::vec3(0, -1, 0.00001f),
                        glm::vec3(maximum, maximum, -maximum)})
        {
            const auto shadow = Render::makeShadowFrame(settings, direction);
            ASSERT_TRUE(shadow);
            const auto normalized = glm::vec3(glm::normalize(glm::dvec3(direction)));
            const auto unit = Render::makeShadowFrame(settings, normalized);
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                {
                    EXPECT_TRUE(std::isfinite(shadow->viewProjection[column][row]));
                    EXPECT_NEAR(shadow->viewProjection[column][row], unit->viewProjection[column][row], 0.00001f);
                }
            const auto halfDepth = (settings.farPlane - settings.nearPlane) * 0.5f;
            const auto near = shadow->viewProjection * glm::vec4(settings.center - normalized * halfDepth, 1);
            const auto far = shadow->viewProjection * glm::vec4(settings.center + normalized * halfDepth, 1);
            EXPECT_NEAR(near.z / near.w, 0, 0.00001f);
            EXPECT_NEAR(far.z / far.w, 1, 0.00001f);
        }
    }

    TEST(SceneFrameCpuTest, InvalidEnabledShadowSettingsReportConfigAndDisabledSettingsAreUnused)
    {
        const auto reject = [](Render::ShadowSettings settings, glm::vec3 direction = {0, 0, -1}) {
            settings.enabled = true;
            try
            {
                Render::makeShadowFrame(settings, direction);
                FAIL() << "Invalid shadow settings were accepted";
            }
            catch (const Error::Exception& error)
            {
                EXPECT_EQ(error.code(), Error::Code::CONFIG);
                EXPECT_NE(error.message().find("Shadow"), std::string::npos);
            }
        };
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        Render::ShadowSettings invalid;
        invalid.center.y = inf;
        reject(invalid);
        invalid = {};
        invalid.halfExtent.x = 0;
        reject(invalid);
        invalid.halfExtent = {6, -1};
        reject(invalid);
        invalid.halfExtent = {nan, 6};
        reject(invalid);
        invalid = {};
        invalid.nearPlane = 0;
        reject(invalid);
        invalid.nearPlane = invalid.farPlane;
        reject(invalid);
        invalid = {};
        invalid.farPlane = inf;
        reject(invalid);
        invalid = {};
        invalid.depthBias = -0.01f;
        reject(invalid);
        invalid.depthBias = nan;
        reject(invalid);
        invalid = {};
        invalid.halfExtent.x = std::numeric_limits<float>::denorm_min();
        reject(invalid);
        reject({}, {0, 0, 0});
        reject({}, {nan, 0, -1});
        reject({}, {0, inf, -1});
        EXPECT_FALSE(Render::makeShadowFrame(invalid, {nan, 0, 0}));
        Scene::Scene scene;
        invalid.enabled = true;
        EXPECT_THROW(Scene::extractSceneFrame(scene, {800, 600}, true, invalid), Error::Exception);
        EXPECT_FALSE(Scene::extractSceneFrame(scene, {0, 600}, true, invalid));
    }

    TEST(SceneFrameCpuTest, ShadowSnapshotIsIndependentOfCameraAndLaterPreviewLightingChanges)
    {
        Scene::Scene scene;
        const Render::Lighting lighting{{0, -3, -4}, {1, 1, 1}, 1, 0.1f};
        scene.setLighting(lighting);
        Render::ShadowSettings settings;
        settings.enabled = true;
        const auto camera = scene.camera();
        auto frame = Scene::extractSceneFrame(scene, {800, 600}, true, settings);
        ASSERT_TRUE(frame && frame->shadow);
        EXPECT_EQ(scene.camera().position(), camera.position());
        EXPECT_EQ(scene.camera().rotation(), camera.rotation());
        EXPECT_EQ(scene.lighting(), lighting);
        EXPECT_TRUE(frame->shadowDraws.empty());
        scene.setCamera(Scene::Camera({5, 6, 7}));
        const auto movedCamera = Scene::extractSceneFrame(scene, {300, 900}, true, settings);
        EXPECT_EQ(frame->shadow->viewProjection, movedCamera->shadow->viewProjection);
        scene.setLighting({{0, 0, -1}, {1, 1, 1}, 1, 0.1f});
        settings.depthBias = 0;
        const auto changed = Scene::extractSceneFrame(scene, {300, 900}, true, settings);
        EXPECT_NE(frame->shadow->viewProjection, changed->shadow->viewProjection);
        const auto input = frame->input();
        frame->shadow->depthBias = 0.25f;
        EXPECT_EQ(input.shadow->depthBias, 0.001f);
        EXPECT_EQ(changed->shadow->depthBias, 0);
        EXPECT_EQ(input.lighting, lighting);
    }
}  // namespace
