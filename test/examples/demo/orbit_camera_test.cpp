#include "examples/demo/orbit_camera.h"

#include <gtest/gtest.h>

#include <cmath>
#include <glm/ext/matrix_transform.hpp>

#include "examples/demo/viewport.h"

namespace
{
    using namespace Incubator;

    TEST(OrbitCameraTest, GesturesStartOnlyOnScenePressAndCancelWithoutRestartingHeldButtons)
    {
        Demo::OrbitCamera orbit;
        Scene::Camera camera({0, 0, 6});
        Demo::UiInput input;
        input.rotate = {true, true, false};
        input.delta = {100, 100};
        input.captureMouse = true;
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600}));
        input.rotate.pressed = false;
        input.captureMouse = false;
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600}));
        input.rotate.pressed = true;
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600}));
        input.rotate.pressed = false;
        input.delta = {20, 0};
        input.captureMouse = true; // 指针越过 UI 区域时，交互所有权仍然保留。
        EXPECT_TRUE(orbit.apply(camera, input, {800, 600}));
        EXPECT_NEAR(camera.position().x, -6 * std::sin(0.1f), 0.00001f);
        EXPECT_NEAR(camera.position().z, 6 * std::cos(0.1f), 0.00001f);
        const auto moved = camera.position();
        input.rotate = {false, false, true};
        EXPECT_TRUE(orbit.apply(camera, input, {800, 600}));
        EXPECT_NE(camera.position(), moved);
        const auto released = camera.position();
        orbit.cancel();
        input.rotate = {true, false, false};
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600}));
        EXPECT_EQ(camera.position(), released);
        input.captureMouse = false;
        input.rotate = {false, true, true};
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600}));
        input.rotate = {true, true, false};
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600}));
        input.delta = {};
        input.rotate = {false, false, true};
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600}));
        EXPECT_EQ(camera.position(), released);
        camera = Scene::Camera({0, 0, 8});
        EXPECT_FALSE(orbit.apply(camera, {}, {800, 600}));
        EXPECT_EQ(camera.position(), glm::vec3(0, 0, 8));
    }

    TEST(OrbitCameraTest, RotateReleaseStartsPanWithoutReusingBatchDelta)
    {
        for (const bool captured : {false, true})
        {
            SCOPED_TRACE(captured);
            Demo::OrbitCamera orbit;
            Scene::Camera camera({0, 0, 6}, {1, 0, 0, 0}, {glm::radians(90.0f), 0.1f, 100});
            Demo::UiInput input;
            input.rotate = {true, true, false};
            ASSERT_FALSE(orbit.apply(camera, input, {800, 600}));

            input.rotate = {false, false, true};
            input.pan = {true, true, false};
            input.delta = {20, 10};
            input.captureMouse = captured;
            ASSERT_TRUE(orbit.apply(camera, input, {800, 600}));
            const auto rotation = glm::angleAxis(-0.1f, glm::vec3(0, 1, 0)) *
                                  glm::angleAxis(-0.05f, glm::vec3(1, 0, 0));
            const auto released = rotation * glm::vec3(0, 0, 6);
            EXPECT_LT(glm::length(camera.position() - released), 0.00001f);
            EXPECT_NEAR(glm::dot(camera.rotation(), rotation), 1.0f, 0.000001f);

            input.rotate = {};
            input.pan.pressed = false;
            input.captureMouse = false;
            input.delta = {30, 15};
            EXPECT_EQ(orbit.apply(camera, input, {800, 600}), !captured);
            const auto expected = captured ? released : released + rotation * glm::vec3(-0.6f, 0.3f, 0);
            EXPECT_LT(glm::length(camera.position() - expected), 0.00001f);
            EXPECT_NEAR(glm::dot(camera.rotation(), rotation), 1.0f, 0.000001f);
        }
    }

    TEST(OrbitCameraTest, PanReleaseStartsRotateWithoutReusingBatchDelta)
    {
        for (const bool captured : {false, true})
        {
            SCOPED_TRACE(captured);
            Demo::OrbitCamera orbit;
            Scene::Camera camera({0, 0, 6}, {1, 0, 0, 0}, {glm::radians(90.0f), 0.1f, 100});
            Demo::UiInput input;
            input.pan = {true, true, false};
            ASSERT_FALSE(orbit.apply(camera, input, {800, 600}));

            input.pan = {false, false, true};
            input.rotate = {true, true, false};
            input.delta = {30, 15};
            input.captureMouse = captured;
            ASSERT_TRUE(orbit.apply(camera, input, {800, 600}));
            const glm::vec3 target(-0.6f, 0.3f, 0);
            const auto released = target + glm::vec3(0, 0, 6);
            EXPECT_LT(glm::length(camera.position() - released), 0.00001f);
            EXPECT_EQ(camera.rotation(), glm::quat(1, 0, 0, 0));

            input.pan = {};
            input.rotate.pressed = false;
            input.captureMouse = false;
            input.delta = {20, 10};
            EXPECT_EQ(orbit.apply(camera, input, {800, 600}), !captured);
            const auto rotation = glm::angleAxis(-0.1f, glm::vec3(0, 1, 0)) *
                                  glm::angleAxis(-0.05f, glm::vec3(1, 0, 0));
            const auto expected = captured ? released : target + rotation * glm::vec3(0, 0, 6);
            EXPECT_LT(glm::length(camera.position() - expected), 0.00001f);
            EXPECT_NEAR(glm::dot(camera.rotation(), captured ? glm::quat(1, 0, 0, 0) : rotation), 1.0f, 0.000001f);
        }
    }

    TEST(OrbitCameraTest, PanUsesWindowCoordinatesAndZoomIsIndependentOfFrameTime)
    {
        Demo::OrbitCamera orbit;
        Scene::Camera camera({0, 0, 6}, {1, 0, 0, 0}, {glm::radians(90.0f), 0.1f, 100});
        Demo::UiInput input;
        input.pan = {true, true, false};
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600}));
        input.pan.pressed = false;
        input.delta = {30, 15};
        EXPECT_TRUE(orbit.apply(camera, input, {800, 600}));
        EXPECT_NEAR(camera.position().x, -0.6f, 0.00001f);
        EXPECT_NEAR(camera.position().y, 0.3f, 0.00001f);
        input = {};
        input.scroll.y = 1;
        EXPECT_TRUE(orbit.apply(camera, input, {800, 600}));
        EXPECT_NEAR(camera.position().z, 6 * std::exp(-0.1f), 0.00001f);
        const auto before = camera.position();
        input.captureMouse = true;
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600}));
        EXPECT_EQ(camera.position(), before);
        input = {};
        input.rotate = {true, true, false};
        orbit.apply(camera, input, {800, 600});
        input.rotate.pressed = false;
        input.delta.y = 100000;
        EXPECT_TRUE(orbit.apply(camera, input, {800, 600}));
        EXPECT_TRUE(std::isfinite(camera.position().y));
        EXPECT_GT(glm::length(camera.position()), 0);
    }

    TEST(OrbitCameraTest, FocusFramesAllWorldCornersInWideAndPortraitProjections)
    {
        const Math::Aabb geometry{{-2, -1, -3}, {4, 2, 1}};
        const auto matrix = glm::translate(glm::mat4(1), glm::vec3(10, -5, 7)) *
                            glm::mat4_cast(glm::quat(glm::vec3(0.3f, 0.8f, -0.4f))) *
                            glm::scale(glm::mat4(1), glm::vec3(-2, 3, 0.5f));
        const auto world = geometry.transformed(matrix);
        ASSERT_TRUE(world);
        for (const Render::FrameExtent extent : {Render::FrameExtent{1600, 400}, {400, 1600}})
        {
            Demo::OrbitCamera orbit;
            Scene::Camera camera({0, 0, 6}, glm::quat(glm::vec3(0.2f, 0.4f, 0)), {glm::radians(45.0f), 1, 2});
            const auto rotation = camera.rotation();
            ASSERT_TRUE(orbit.focus(camera, world, extent));
            EXPECT_EQ(camera.rotation(), rotation);
            EXPECT_FLOAT_EQ(camera.perspective().verticalFov, glm::radians(45.0f));
            const auto matrices = camera.matrices(extent);
            for (int i = 0; i < 8; ++i)
            {
                const glm::vec3 p((i & 1) ? world->max.x : world->min.x,
                                  (i & 2) ? world->max.y : world->min.y,
                                  (i & 4) ? world->max.z : world->min.z);
                const auto clip = matrices.projection * matrices.view * glm::vec4(p, 1);
                ASSERT_GT(clip.w, 0);
                EXPECT_LE(std::abs(clip.x), clip.w + 0.0001f);
                EXPECT_LE(std::abs(clip.y), clip.w + 0.0001f);
                EXPECT_GE(clip.z, -0.0001f);
                EXPECT_LE(clip.z, clip.w + 0.0001f);
            }
        }
    }

    TEST(OrbitCameraTest, UnavailableFocusPreservesGestureAndPointBoundsProduceValidCamera)
    {
        Demo::OrbitCamera orbit;
        Scene::Camera camera({0, 0, 6});
        Demo::UiInput input;
        input.rotate = {true, true, false};
        orbit.apply(camera, input, {800, 600});
        EXPECT_FALSE(orbit.focus(camera, std::nullopt, {800, 600}));
        EXPECT_EQ(camera.position(), glm::vec3(0, 0, 6));
        input.rotate.pressed = false;
        input.delta.x = 10;
        EXPECT_TRUE(orbit.apply(camera, input, {800, 600}));
        ASSERT_TRUE(orbit.focus(camera, Math::Aabb{{3, 4, 5}, {3, 4, 5}}, {800, 600}));
        EXPECT_GT(camera.perspective().nearPlane, 0);
        EXPECT_GT(camera.perspective().farPlane, camera.perspective().nearPlane);
        EXPECT_FALSE(orbit.apply(camera, input, {800, 600})); // 焦点丢失结束了旧手势。
    }

}  // namespace

TEST(ViewportExtentTest, UsesFramebufferRatioAndOneRoundingRule)
{
    using Incubator::Demo::viewportExtent;
    EXPECT_EQ(viewportExtent(123.9, 80.4, 1, 1), (Incubator::Render::FrameExtent{123, 80}));
    EXPECT_EQ(viewportExtent(123.9, 80.4, 1.5, 2), (Incubator::Render::FrameExtent{185, 160}));
    EXPECT_EQ(viewportExtent(0.1, 0.1, 1, 1), (Incubator::Render::FrameExtent{1, 1}));
    EXPECT_EQ(viewportExtent(0, 10, 1, 1), (Incubator::Render::FrameExtent{}));
    EXPECT_EQ(viewportExtent(10, 10, 0, 1), (Incubator::Render::FrameExtent{}));
}
