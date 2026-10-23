#include "runtime/core/math/aabb.h"

#include <gtest/gtest.h>

#include <array>
#include <glm/ext/matrix_transform.hpp>
#include <limits>

namespace
{
    using Incubator::Math::Aabb;

    void expectBounds(const Aabb& actual, const glm::vec3& min, const glm::vec3& max)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            EXPECT_FLOAT_EQ(actual.min[axis], min[axis]);
            EXPECT_FLOAT_EQ(actual.max[axis], max[axis]);
        }
    }

    TEST(AabbTest, IncludesPointsMergesBoundsAndContainsBoundary)
    {
        Aabb bounds{{1.0f, 2.0f, 3.0f}, {1.0f, 2.0f, 3.0f}};
        EXPECT_TRUE(bounds.contains({1.0f, 2.0f, 3.0f}));
        EXPECT_FALSE(bounds.contains({0.0f, 2.0f, 3.0f}));
        bounds.include({-2.0f, 4.0f, 1.0f});
        expectBounds(bounds, {-2.0f, 2.0f, 1.0f}, {1.0f, 4.0f, 3.0f});
        bounds.merge({{-1.0f, -3.0f, 2.0f}, {5.0f, 3.0f, 7.0f}});
        expectBounds(bounds, {-2.0f, -3.0f, 1.0f}, {5.0f, 4.0f, 7.0f});
        EXPECT_TRUE(bounds.contains(bounds.min));
        EXPECT_TRUE(bounds.contains(bounds.max));
        for (const glm::vec3 outside : {glm::vec3(-3, 0, 2), glm::vec3(0, 5, 2), glm::vec3(0, 0, 8)})
        {
            EXPECT_FALSE(bounds.contains(outside));
        }
    }

    TEST(AabbTest, AffineTransformsMatchFixedBoundsAndCoverAllEightCorners)
    {
        const Aabb bounds{{-1.0f, -2.0f, -3.0f}, {2.0f, 4.0f, 5.0f}};
        glm::mat4 rotation(1.0f);
        rotation[0] = {0.0f, 1.0f, 0.0f, 0.0f};
        rotation[1] = {-1.0f, 0.0f, 0.0f, 0.0f};
        glm::mat4 shear(1.0f);
        shear[1][0] = -2.0f;
        shear[0][2] = 3.0f;
        struct Example
        {
            glm::mat4 matrix;
            Aabb expected;
        };
        const std::array<Example, 5> examples{{
            {glm::translate(glm::mat4(1.0f), {3.0f, -1.0f, 2.0f}), {{2, -3, -1}, {5, 3, 7}}},
            {rotation, {{-4, -1, -3}, {2, 2, 5}}},
            {glm::scale(glm::mat4(1.0f), {2.0f, 3.0f, 4.0f}), {{-2, -6, -12}, {4, 12, 20}}},
            {glm::scale(glm::mat4(1.0f), {-2.0f, 0.0f, 3.0f}), {{-4, 0, -9}, {2, 0, 15}}},
            {shear, {{-9, -2, -6}, {6, 4, 11}}},
        }};
        for (size_t i = 0; i < examples.size(); ++i)
        {
            SCOPED_TRACE(i);
            const auto& example = examples[i];
            const auto moved = bounds.transformed(example.matrix);
            ASSERT_TRUE(moved);
            expectBounds(*moved, example.expected.min, example.expected.max);
            for (int corner = 0; corner < 8; ++corner)
            {
                const glm::vec3 point((corner & 1) ? 2 : -1, (corner & 2) ? 4 : -2, (corner & 4) ? 5 : -3);
                EXPECT_TRUE(moved->contains(glm::vec3(example.matrix * glm::vec4(point, 1.0f))));
            }
        }
    }

    TEST(AabbTest, ZeroScaleProducesValidPointInsteadOfEmptyBounds)
    {
        const Aabb bounds{{-1, -2, -3}, {2, 4, 5}};
        const auto matrix =
            glm::translate(glm::mat4(1.0f), {3.0f, 4.0f, 5.0f}) * glm::scale(glm::mat4(1.0f), glm::vec3(0.0f));
        const auto moved = bounds.transformed(matrix);
        ASSERT_TRUE(moved);
        expectBounds(*moved, {3, 4, 5}, {3, 4, 5});
        EXPECT_TRUE(moved->contains({3, 4, 5}));
    }

    TEST(AabbTest, NonAffineOrNonFiniteDerivedBoundsAreUnavailable)
    {
        const Aabb bounds{{-1, -2, -3}, {2, 4, 5}};
        glm::mat4 matrix(1.0f);
        matrix[0][3] = 0.5f;
        EXPECT_FALSE(bounds.transformed(matrix));
        matrix = glm::mat4(1.0f);
        matrix[3][3] = 2.0f;
        EXPECT_FALSE(bounds.transformed(matrix));
        matrix = glm::mat4(1.0f);
        matrix[3][0] = std::numeric_limits<float>::quiet_NaN();
        EXPECT_FALSE(bounds.transformed(matrix));
        matrix = glm::scale(glm::mat4(1.0f), glm::vec3(std::numeric_limits<float>::max()));
        EXPECT_FALSE(bounds.transformed(matrix));
    }
}  // namespace
