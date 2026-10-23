#include "runtime/core/math/aabb.h"

#include <cmath>
#include <glm/common.hpp>
#include <glm/vec4.hpp>

namespace Incubator
{
    namespace Math
    {
        void Aabb::include(const glm::vec3& point) noexcept
        {
            min = glm::min(min, point);
            max = glm::max(max, point);
        }

        void Aabb::merge(const Aabb& other) noexcept
        {
            include(other.min);
            include(other.max);
        }

        bool Aabb::contains(const glm::vec3& point) const noexcept
        {
            return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y && point.z >= min.z &&
                   point.z <= max.z;
        }

        std::optional<Aabb> Aabb::transformed(const glm::mat4& matrix) const noexcept
        {
            if (matrix[0][3] != 0.0f || matrix[1][3] != 0.0f || matrix[2][3] != 0.0f || matrix[3][3] != 1.0f)
            {
                return std::nullopt;
            }

            std::optional<Aabb> result;
            for (int corner = 0; corner < 8; ++corner)
            {
                const glm::vec3 point((corner & 1) ? max.x : min.x, (corner & 2) ? max.y : min.y,
                                      (corner & 4) ? max.z : min.z);
                const glm::vec3 moved(matrix * glm::vec4(point, 1.0f));
                if (!std::isfinite(moved.x) || !std::isfinite(moved.y) || !std::isfinite(moved.z))
                {
                    return std::nullopt;
                }
                if (result)
                {
                    result->include(moved);
                }
                else
                {
                    result = Aabb{moved, moved};
                }
            }
            return result;
        }
    }  // namespace Math
}  // namespace Incubator
