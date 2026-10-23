#include "runtime/scene/frustum.h"

#include <cmath>

namespace Incubator::Scene
{
    namespace
    {
        // Planes have unit normals, so the tolerance is in world-space units.
        constexpr double BOUNDARY_TOLERANCE = 0.00001;
    }  // namespace

    Frustum::Frustum(const Render::CameraMatrices& camera) noexcept
    {
        const auto clip = camera.projection * camera.view;
        const glm::dvec4 x(clip[0][0], clip[1][0], clip[2][0], clip[3][0]);
        const glm::dvec4 y(clip[0][1], clip[1][1], clip[2][1], clip[3][1]);
        const glm::dvec4 z(clip[0][2], clip[1][2], clip[2][2], clip[3][2]);
        const glm::dvec4 w(clip[0][3], clip[1][3], clip[2][3], clip[3][3]);
        // -w <= x,y <= w; 0 <= z <= w. In particular, the near plane is z, not w + z.
        planes_ = {w + x, w - x, w + y, w - y, z, w - z};
        for (auto& plane : planes_)
        {
            const double length = std::hypot(plane.x, plane.y, plane.z);
            if (!std::isfinite(length) || length == 0.0 || !std::isfinite(plane.w))
                return;
            plane /= length;
        }
        usable_ = true;
    }

    bool Frustum::intersects(const Math::Aabb& bounds) const noexcept
    {
        if (!usable_)
            return true;
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(bounds.min[axis]) || !std::isfinite(bounds.max[axis]))
                return true;

        for (const auto& plane : planes_)
        {
            // The corner furthest toward the inward normal maximizes signed distance.
            // If even that corner lies outside, the entire box is outside this plane.
            const glm::dvec3 positive(plane.x >= 0 ? bounds.max.x : bounds.min.x,
                                      plane.y >= 0 ? bounds.max.y : bounds.min.y,
                                      plane.z >= 0 ? bounds.max.z : bounds.min.z);
            const double distance = plane.x * positive.x + plane.y * positive.y + plane.z * positive.z + plane.w;
            if (distance < -BOUNDARY_TOLERANCE)
                return false;
        }
        return true;
    }
}  // namespace Incubator::Scene
