#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <optional>

namespace Incubator
{
    namespace Math
    {
        // Finite, ordered endpoints. Equal endpoints are valid; use optional<Aabb> for an empty collection.
        struct Aabb
        {
            glm::vec3 min;
            glm::vec3 max;

            void include(const glm::vec3& point) noexcept;
            void merge(const Aabb& other) noexcept;
            bool contains(const glm::vec3& point) const noexcept;

            // No perspective divide. Unavailable bounds must retain the original drawing path.
            std::optional<Aabb> transformed(const glm::mat4& matrix) const noexcept;
        };
    }  // namespace Math
}  // namespace Incubator
