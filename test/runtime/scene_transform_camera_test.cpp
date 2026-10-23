#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <glm/gtc/constants.hpp>
#include <limits>

#include "runtime/core/error/error.h"
#include "runtime/scene/camera.h"
#include "runtime/scene/transform.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Scene;
    constexpr float EPSILON = 0.00001f;

    void expectPoint(const glm::mat4& matrix, glm::vec4 input, glm::vec4 expected)
    {
        const auto actual = matrix * input;
        for (int axis = 0; axis < 4; ++axis)
            EXPECT_NEAR(actual[axis], expected[axis], EPSILON) << "axis " << axis;
    }

    template <class Operation>
    void expectConfig(Operation operation)
    {
        try
        {
            operation();
            FAIL() << "Accepted invalid scene input";
        }
        catch (const Error::Exception& error)
        {
            EXPECT_EQ(error.code(), Error::Code::CONFIG);
        }
    }

    TEST(SceneTransformTest, DefaultsSettersAndCopiesOwnTheirValues)
    {
        Transform transform;
        EXPECT_EQ(transform.position(), glm::vec3(0));
        EXPECT_EQ(transform.rotation(), glm::quat(1, 0, 0, 0));
        EXPECT_EQ(transform.scale(), glm::vec3(1));
        EXPECT_EQ(transform.matrix(), glm::mat4(1));
        auto copy = transform;
        transform.setPosition({1, 2, 3});
        transform.setScale({2, 3, 4});
        EXPECT_EQ(copy.matrix(), glm::mat4(1));
        EXPECT_EQ(transform.position(), glm::vec3(1, 2, 3));
        EXPECT_EQ(transform.scale(), glm::vec3(2, 3, 4));
        expectPoint(transform.matrix(), {1, 1, 1, 1}, {3, 5, 7, 1});
        copy = transform;
        transform.setPosition({4, 5, 6});
        EXPECT_EQ(copy.position(), glm::vec3(1, 2, 3));
    }

    TEST(SceneTransformTest, NoncommutingTranslationRotationAndScaleUseTrs)
    {
        // 非均匀缩放后、平移前应用 Rz(90)：(x,y,z) -> (-y,x,z)。
        Transform transform({1, 2, 3}, {1, 0, 0, 1}, {2, 3, 4});
        expectPoint(transform.matrix(), {1, 2, 3, 1}, {-5, 4, 15, 1});
        expectPoint(transform.matrix(), {1, 2, 3, 0}, {-6, 2, 12, 0});
        transform.setRotation({1, 1, 0, 0});  // Rx(90).
        expectPoint(transform.matrix(), {1, 2, 3, 1}, {3, -10, 9, 1});
    }

    TEST(SceneTransformTest, FiniteZeroAndNegativeScaleRemainValid)
    {
        Transform transform({1, 2, 3}, {1, 0, 0, 0}, {0, -2, 3});
        expectPoint(transform.matrix(), {9, 2, 3, 1}, {1, -2, 12, 1});
        transform.setScale({-1, 0, 0});
        expectPoint(transform.matrix(), {2, 9, 9, 1}, {-1, 2, 3, 1});
    }

    TEST(ScenePoseInputTest, TransformAndCameraNormalizeTheSameFiniteNonzeroRotations)
    {
        for (const float magnitude :
             {1.0f, 2.0f, -3.0f, std::numeric_limits<float>::max(), std::numeric_limits<float>::denorm_min()})
        {
            SCOPED_TRACE(magnitude);
            const glm::quat rotation(magnitude, 0, 0, magnitude);
            Transform transform({}, rotation);
            Camera camera({}, rotation);
            EXPECT_NEAR(glm::length(transform.rotation()), 1, EPSILON);
            EXPECT_EQ(transform.rotation(), camera.rotation());
            expectPoint(transform.matrix(), {1, 0, 0, 1}, {0, 1, 0, 1});
            expectPoint(camera.matrices({800, 600}).view, {0, 1, -2, 1}, {1, 0, -2, 1});
            Transform setTransform;
            Camera setCamera;
            setTransform.setRotation(rotation);
            setCamera.setRotation(rotation);
            EXPECT_EQ(setTransform.rotation(), transform.rotation());
            EXPECT_EQ(setCamera.rotation(), camera.rotation());
            EXPECT_EQ(setTransform.matrix(), transform.matrix());
        }
    }

    TEST(ScenePoseInputTest, InvalidRotationConstructionAndMutationFailWithoutChangingValues)
    {
        Transform transform({1, 2, 3}, {1, 0, 0, 1}, {2, 3, 4});
        Camera camera({1, 2, 3}, {1, 0, 1, 0});
        const auto model = transform.matrix();
        const auto view = camera.matrices({800, 600}).view;
        const auto reject = [&](glm::quat rotation) {
            expectConfig([&] { Transform value({}, rotation); });
            expectConfig([&] { Camera value({}, rotation); });
            expectConfig([&] { transform.setRotation(rotation); });
            expectConfig([&] { camera.setRotation(rotation); });
            EXPECT_EQ(transform.matrix(), model);
            EXPECT_EQ(camera.matrices({800, 600}).view, view);
        };
        reject({0, 0, 0, 0});
        for (const float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                    -std::numeric_limits<float>::infinity()})
            for (int axis = 0; axis < 4; ++axis)
            {
                auto rotation = glm::quat(1, 0, 0, 0);
                rotation[axis] = invalid;
                reject(rotation);
            }
    }

    TEST(ScenePoseInputTest, NonfinitePositionAndScaleAreRejectedAtInput)
    {
        Transform transform;
        Camera camera;
        for (const float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                    -std::numeric_limits<float>::infinity()})
            for (int axis = 0; axis < 3; ++axis)
            {
                auto value = glm::vec3(1);
                value[axis] = invalid;
                expectConfig([&] { Transform bad(value); });
                expectConfig([&] { Transform bad({}, {1, 0, 0, 0}, value); });
                expectConfig([&] { Camera bad(value); });
                expectConfig([&] { transform.setPosition(value); });
                expectConfig([&] { transform.setScale(value); });
                expectConfig([&] { camera.setPosition(value); });
                EXPECT_EQ(transform.position(), glm::vec3(0));
                EXPECT_EQ(transform.scale(), glm::vec3(1));
                EXPECT_EQ(camera.position(), glm::vec3(0));
            }
    }

    TEST(SceneCameraTest, DefaultsLookAlongNegativeZWithUpPositiveY)
    {
        Camera camera;
        const auto matrices = camera.matrices({800, 600});
        EXPECT_EQ(camera.position(), glm::vec3(0));
        EXPECT_EQ(camera.rotation(), glm::quat(1, 0, 0, 0));
        EXPECT_EQ(matrices.view, glm::mat4(1));
        EXPECT_LT(matrices.projection[1][1], 0);
        const auto front = matrices.projection * glm::vec4(0, 0, -1, 1);
        EXPECT_GT(front.w, 0);
        EXPECT_GT(front.z / front.w, 0);
        EXPECT_LT(front.z / front.w, 1);
        const auto up = matrices.projection * glm::vec4(0, 1, -2, 1);
        EXPECT_LT(up.y / up.w, 0);
    }

    TEST(SceneCameraTest, WorldRigidPoseIsInvertedIncludingRotationAndTranslation)
    {
        // Ry(90) * Rz(90)：世界前方向为 -X，上方向为 +Z，右方向为 +Y。
        Camera camera({3, 4, 5}, {0.5f, 0.5f, 0.5f, 0.5f});
        const auto view = camera.matrices({800, 600}).view;
        expectPoint(view, {3, 4, 5, 1}, {0, 0, 0, 1});
        expectPoint(view, {1, 4, 5, 1}, {0, 0, -2, 1});
        expectPoint(view, {3, 4, 6, 1}, {0, 1, 0, 1});
        expectPoint(view, {3, 5, 5, 1}, {1, 0, 0, 1});
        camera.setPosition({0, 0, 3});
        camera.setRotation({1, 0, 0, 0});
        expectPoint(camera.matrices({800, 600}).view, {0, 0, 0, 1}, {0, 0, -3, 1});
    }

    TEST(SceneCameraTest, ActualAspectFovDepthAndFinalYAdaptationAreApplied)
    {
        Camera camera({}, {1, 0, 0, 0}, {glm::half_pi<float>(), 1, 11});
        const auto wide = camera.matrices({800, 400});
        const auto square = camera.matrices({400, 400});
        EXPECT_NEAR(wide.projection[0][0], 0.5f, EPSILON);
        EXPECT_NEAR(square.projection[0][0], 1, EPSILON);
        EXPECT_NEAR(wide.projection[1][1], -1, EPSILON);
        EXPECT_EQ(wide.projection[1][1], square.projection[1][1]);
        for (const auto& [distance, depth] : {std::pair{1.0f, 0.0f}, std::pair{11.0f, 1.0f}})
        {
            const auto projected = wide.projection * glm::vec4(0, 0, -distance, 1);
            EXPECT_NEAR(projected.z / projected.w, depth, EPSILON);
        }
        auto copy = camera;
        camera.setPerspective({glm::half_pi<float>() / 2, 0.5f, 20});
        EXPECT_NE(camera.matrices({800, 400}).projection, wide.projection);
        EXPECT_EQ(copy.matrices({800, 400}).projection, wide.projection);
        EXPECT_FLOAT_EQ(camera.perspective().nearPlane, 0.5f);
        EXPECT_FLOAT_EQ(camera.perspective().farPlane, 20);
    }

    TEST(SceneCameraTest, InvalidPerspectiveConstructionAndMutationPreservePreviousState)
    {
        Camera camera;
        const auto projection = camera.matrices({800, 600}).projection;
        const auto reject = [&](Perspective perspective) {
            expectConfig([&] { Camera bad({}, {1, 0, 0, 0}, perspective); });
            expectConfig([&] { camera.setPerspective(perspective); });
            EXPECT_EQ(camera.matrices({800, 600}).projection, projection);
        };
        for (const float fov : {-1.0f, 0.0f, glm::pi<float>(), 4.0f})
            reject({fov, 0.1f, 100});
        for (const float nearPlane : {-1.0f, 0.0f, 100.0f, 101.0f})
            reject({glm::half_pi<float>(), nearPlane, 100});
        reject({glm::half_pi<float>(), 0.1f, -1});
        for (const float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                    -std::numeric_limits<float>::infinity()})
        {
            reject({invalid, 0.1f, 100});
            reject({glm::half_pi<float>(), invalid, 100});
            reject({glm::half_pi<float>(), 0.1f, invalid});
        }
    }

    TEST(SceneCameraTest, ZeroExtentIsRejectedBeforeComputingProjection)
    {
        Camera camera;
        for (const Render::FrameExtent extent : {Render::FrameExtent{0, 0}, {0, 600}, {800, 0}})
            expectConfig([&] { camera.matrices(extent); });
        EXPECT_EQ(camera.matrices({800, 600}).view, glm::mat4(1));
    }
}  // namespace
