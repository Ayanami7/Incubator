#pragma once

#include <array>
#include <glm/vec4.hpp>

#include "runtime/core/math/aabb.h"
#include "runtime/render/frame_input.h"

namespace Incubator::Scene
{
    // Six inward planes from final projection * view, with zero-to-one depth.
    // Pure CPU value; an unusable frustum conservatively keeps every candidate.
    class Frustum
    {
    public:
        explicit Frustum(const Render::CameraMatrices& camera) noexcept;

        // True means possibly intersecting; contact and non-finite bounds are retained.
        bool intersects(const Math::Aabb& bounds) const noexcept;

    private:
        std::array<glm::dvec4, 6> planes_{};
        bool usable_ = false;
    };
}  // namespace Incubator::Scene
