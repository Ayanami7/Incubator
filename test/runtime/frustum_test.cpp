#include "runtime/scene/frustum.h"

#include <gtest/gtest.h>

#include <array>
#include <glm/gtc/constants.hpp>
#include <limits>

#include "runtime/scene/camera.h"

namespace
{
    using namespace Incubator;
    using Math::Aabb;
    using Scene::Camera;
    using Scene::Frustum;

    Camera squareCamera()
    {
        return Camera({}, {1, 0, 0, 0}, {glm::half_pi<float>(), 1, 11});
    }

    TEST(FrustumTest, PerspectiveRejectsAllSixOutsideDirectionsAndBehindCamera)
    {
        const Frustum frustum(squareCamera().matrices({400, 400}));
        const std::array<Aabb, 7> outside{{
            {{-4, -0.1f, -2.1f}, {-3, 0.1f, -1.9f}},
            {{3, -0.1f, -2.1f}, {4, 0.1f, -1.9f}},
            {{-0.1f, -4, -2.1f}, {0.1f, -3, -1.9f}},
            {{-0.1f, 3, -2.1f}, {0.1f, 4, -1.9f}},
            {{-0.1f, -0.1f, -0.5f}, {0.1f, 0.1f, -0.25f}},
            {{-0.1f, -0.1f, -13}, {0.1f, 0.1f, -12}},
            {{-0.1f, -0.1f, 1}, {0.1f, 0.1f, 2}},
        }};
        for (size_t i = 0; i < outside.size(); ++i)
        {
            SCOPED_TRACE(i);
            EXPECT_FALSE(frustum.intersects(outside[i]));
        }
        EXPECT_TRUE(frustum.intersects({{-0.5f, -0.5f, -3}, {0.5f, 0.5f, -2}}));
    }

    TEST(FrustumTest, AllPlanesKeepCrossingsContactAndSmallBoundaryPerturbations)
    {
        const Frustum frustum({glm::mat4(1), glm::mat4(1)});
        const std::array<glm::vec3, 6> contact{
            {{-1, 0, 0.5f}, {1, 0, 0.5f}, {0, -1, 0.5f}, {0, 1, 0.5f}, {0, 0, 0}, {0, 0, 1}}};
        const std::array<glm::vec3, 6> outward{{{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}}};
        for (size_t i = 0; i < contact.size(); ++i)
        {
            SCOPED_TRACE(i);
            const auto point = contact[i];
            EXPECT_TRUE(frustum.intersects({point, point}));
            EXPECT_TRUE(frustum.intersects({point - glm::vec3(0.1f), point + glm::vec3(0.1f)}));
            const auto justOutside = point + outward[i] * 0.000001f;
            EXPECT_TRUE(frustum.intersects({justOutside, justOutside}));
            const auto justInside = point - outward[i] * 0.000001f;
            EXPECT_TRUE(frustum.intersects({justInside, justInside}));
            const auto separated = point + outward[i] * 0.001f;
            EXPECT_FALSE(frustum.intersects({separated, separated}));
        }
    }

    TEST(FrustumTest, NearAndFarPlanesFollowZeroToOneDepth)
    {
        const Frustum clip({glm::mat4(1), glm::mat4(1)});
        // A negative-one-to-one near plane would incorrectly keep this box.
        EXPECT_FALSE(clip.intersects({{-0.1f, -0.1f, -0.5f}, {0.1f, 0.1f, -0.25f}}));
        EXPECT_TRUE(clip.intersects({{0, 0, 0}, {0, 0, 0}}));
        EXPECT_TRUE(clip.intersects({{0, 0, 1}, {0, 0, 1}}));
        EXPECT_FALSE(clip.intersects({{0, 0, 1.1f}, {0, 0, 2}}));
        const Frustum perspective(squareCamera().matrices({400, 400}));
        EXPECT_TRUE(perspective.intersects({{0, 0, -1}, {0, 0, -1}}));
        EXPECT_TRUE(perspective.intersects({{0, 0, -11}, {0, 0, -11}}));
        EXPECT_FALSE(perspective.intersects({{0, 0, -0.75f}, {0, 0, -0.75f}}));
        EXPECT_FALSE(perspective.intersects({{0, 0, -11.1f}, {0, 0, -11.1f}}));
    }

    TEST(FrustumTest, CameraInsideAndContainingBoxesDoNotRequireAnInsideCorner)
    {
        const Frustum frustum(squareCamera().matrices({400, 400}));
        EXPECT_TRUE(frustum.intersects({{-2, -2, -2}, {2, 2, 2}}));
        const Aabb enclosing{{-20, -20, -20}, {20, 20, 20}};
        EXPECT_TRUE(frustum.intersects(enclosing));
        for (int corner = 0; corner < 8; ++corner)
        {
            const glm::vec3 point((corner & 1) ? 20 : -20, (corner & 2) ? 20 : -20, (corner & 4) ? 20 : -20);
            EXPECT_FALSE(frustum.intersects({point, point}));
        }
    }

    TEST(FrustumTest, CurrentPoseFovAndExtentChangeWorldVisibility)
    {
        auto camera = squareCamera();
        const Aabb wideOnly{{3, 0, -2}, {3, 0, -2}};
        EXPECT_FALSE(Frustum(camera.matrices({400, 400})).intersects(wideOnly));
        EXPECT_TRUE(Frustum(camera.matrices({800, 400})).intersects(wideOnly));
        const Aabb high{{0, 1.5f, -2}, {0, 1.5f, -2}};
        EXPECT_TRUE(Frustum(camera.matrices({400, 400})).intersects(high));
        camera.setPerspective({glm::quarter_pi<float>(), 1, 11});
        EXPECT_FALSE(Frustum(camera.matrices({400, 400})).intersects(high));
        EXPECT_FALSE(Frustum(camera.matrices({800, 400})).intersects(wideOnly));

        camera = squareCamera();
        const Aabb translatedFront{{0, 0, 1}, {0, 0, 1}};
        EXPECT_FALSE(Frustum(camera.matrices({400, 400})).intersects(translatedFront));
        camera.setPosition({0, 0, 3});
        EXPECT_TRUE(Frustum(camera.matrices({400, 400})).intersects(translatedFront));
        camera.setPosition({3, 4, 5});
        camera.setRotation({0.5f, 0.5f, 0.5f, 0.5f});  // Forward -X, up +Z.
        const Frustum rotated(camera.matrices({400, 400}));
        EXPECT_TRUE(rotated.intersects({{1, 4, 5}, {1, 4, 5}}));
        EXPECT_FALSE(rotated.intersects({{5, 4, 5}, {5, 4, 5}}));
        EXPECT_FALSE(rotated.intersects({{1, 4, 8}, {1, 4, 8}}));
    }

    TEST(FrustumTest, FinalProjectionIsConsumedWithoutAnotherYAdaptation)
    {
        Render::CameraMatrices camera{glm::mat4(1), glm::mat4(1)};
        camera.projection[1][1] = -1;
        camera.projection[3][1] = 0.5f;
        const Frustum frustum(camera);
        EXPECT_TRUE(frustum.intersects({{0, 1.25f, 0.5f}, {0, 1.25f, 0.5f}}));
        EXPECT_FALSE(frustum.intersects({{0, -1, 0.5f}, {0, -1, 0.5f}}));
    }

    TEST(FrustumTest, UnusablePlanesAndNonfiniteBoundsKeepCandidates)
    {
        const Aabb outside{{100, 100, 100}, {101, 101, 101}};
        for (const float invalid :
             {0.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
        {
            Render::CameraMatrices camera{glm::mat4(1), glm::mat4(1)};
            camera.projection[0][0] = invalid;
            EXPECT_TRUE(Frustum(camera).intersects(outside));
        }
        const float large = std::numeric_limits<float>::max();
        EXPECT_TRUE(Frustum({glm::mat4(large), glm::mat4(large)}).intersects(outside));
        const Frustum valid(squareCamera().matrices({400, 400}));
        auto unavailable = outside;
        unavailable.min.z = std::numeric_limits<float>::quiet_NaN();
        EXPECT_TRUE(valid.intersects(unavailable));
        unavailable = outside;
        unavailable.max.z = std::numeric_limits<float>::infinity();
        EXPECT_TRUE(valid.intersects(unavailable));
    }
}  // namespace
