#include "gizmo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <glm/ext/scalar_constants.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <limits>
#include <utility>

#include "app/common/ui_input.h"
#include "runtime/scene/scene.h"

namespace Incubator::Editor
{
    namespace
    {
        constexpr int ROTATE_RING_SEGMENTS = 48;

        bool isValidViewport(Render::FrameExtent extent, const App::ViewportRegion& region) noexcept
        {
            return extent.width > 0 && extent.height > 0 && std::isfinite(region.origin.x) &&
                   std::isfinite(region.origin.y) && std::isfinite(region.size.x) && std::isfinite(region.size.y) &&
                   region.size.x > 0.0 && region.size.y > 0.0;
        }

        bool isFiniteVec3(const glm::vec3& value) noexcept
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        std::optional<glm::vec2> clipToScreen(const glm::dvec4& clip, const App::ViewportRegion& region) noexcept
        {
            if (clip.w <= 0.0)
                return std::nullopt;
            const double ndcX = clip.x / clip.w;
            const double ndcY = clip.y / clip.w;
            if (!std::isfinite(ndcX) || !std::isfinite(ndcY))
                return std::nullopt;
            // Camera::matrices 已完成 Vulkan Y 轴适配，此处直接将 [-1, 1] 映射到视口逻辑区域。
            const double sx = region.origin.x + (ndcX + 1.0) * 0.5 * region.size.x;
            const double sy = region.origin.y + (ndcY + 1.0) * 0.5 * region.size.y;
            if (!std::isfinite(sx) || !std::isfinite(sy))
                return std::nullopt;
            return glm::vec2(static_cast<float>(sx), static_cast<float>(sy));
        }

        double distancePointToSegment(const glm::dvec2& point, const glm::dvec2& start,
                                      const glm::dvec2& end) noexcept
        {
            const glm::dvec2 delta = end - start;
            const double lenSq = glm::dot(delta, delta);
            const double t = lenSq > 0.0 ? std::clamp(glm::dot(point - start, delta) / lenSq, 0.0, 1.0) : 0.0;
            return glm::length(point - (start + delta * t));
        }

        size_t axisIndex(GizmoAxis axis) noexcept
        {
            switch (axis)
            {
                case GizmoAxis::X:
                    return 0;
                case GizmoAxis::Y:
                    return 1;
                case GizmoAxis::Z:
                    return 2;
                case GizmoAxis::None:
                    break;
            }
            return 3;
        }
    }  // namespace

    std::optional<float> computeGizmoWorldLength(const glm::vec3& center, const Scene::Camera& camera,
                                                 Render::FrameExtent extent, const App::ViewportRegion& region,
                                                 float screenLength) noexcept
    {
        if (!isFiniteVec3(center) || !isValidViewport(extent, region) || !std::isfinite(screenLength) ||
            screenLength <= 0.0f)
            return std::nullopt;

        const auto matrices = camera.matrices(extent);
        const glm::dvec4 viewCenter = glm::dmat4(matrices.view) * glm::dvec4(glm::dvec3(center), 1.0);
        const double zView = -viewCenter.z;
        const double nearPlane = static_cast<double>(camera.perspective().nearPlane);
        if (!std::isfinite(zView) || !std::isfinite(nearPlane) || zView < nearPlane)
            return std::nullopt;

        const double safeDepth = std::max({zView, nearPlane, 0.1});
        const double halfFovTan = std::tan(static_cast<double>(camera.perspective().verticalFov) * 0.5);
        const double worldLength =
            safeDepth * 2.0 * halfFovTan * (static_cast<double>(screenLength) / region.size.y);
        if (!std::isfinite(worldLength) || worldLength <= 0.0)
            return std::nullopt;
        return static_cast<float>(worldLength);
    }

    ProjectedGizmo projectGizmo(const Scene::Scene& scene, Scene::EntityId selected, GizmoMode mode,
                                Render::FrameExtent extent, const App::ViewportRegion& region, float screenLength)
    {
        const auto* entity = scene.find(selected);
        if (!entity || !entity->visible || !isValidViewport(extent, region))
            return {};

        const glm::vec3 center = entity->transform.position();
        const auto worldLengthOpt = computeGizmoWorldLength(center, scene.camera(), extent, region, screenLength);
        if (!worldLengthOpt)
            return {};

        const double worldLength = static_cast<double>(*worldLengthOpt);
        const glm::dvec3 origin(center);
        const auto matrices = scene.camera().matrices(extent);
        const glm::dmat4 viewProjection = glm::dmat4(matrices.projection) * glm::dmat4(matrices.view);

        ProjectedGizmo result;
        result.origin = center;
        result.worldLength = *worldLengthOpt;
        result.visible = true;

        auto emitWorldSegment = [&](GizmoAxis axis, const glm::dvec3& startWorld, const glm::dvec3& endWorld) {
            const glm::dvec4 clipStart = viewProjection * glm::dvec4(startWorld, 1.0);
            const glm::dvec4 clipEnd = viewProjection * glm::dvec4(endWorld, 1.0);
            const auto clipped = clipHomogeneousSegment(clipStart, clipEnd);
            if (!clipped)
                return;
            const auto p0 = clipToScreen(clipped->first, region);
            const auto p1 = clipToScreen(clipped->second, region);
            if (!p0 || !p1)
                return;
            result.segments.push_back({axis, {*p0, *p1}});
        };

        struct AxisBasis
        {
            GizmoAxis axis;
            glm::dvec3 dir;
            glm::dvec3 u;
            glm::dvec3 v;
        };

        std::array<AxisBasis, 3> bases = {{
            {GizmoAxis::X, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}},
            {GizmoAxis::Y, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}},
            {GizmoAxis::Z, {0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}},
        }};

        if (mode == GizmoMode::Scale)
        {
            const glm::dquat rotation = glm::normalize(glm::dquat(entity->transform.rotation()));
            for (auto& basis : bases)
            {
                basis.dir = glm::normalize(rotation * basis.dir);
                basis.u = glm::normalize(rotation * basis.u);
                basis.v = glm::normalize(rotation * basis.v);
            }
        }

        if (mode == GizmoMode::Translate)
        {
            result.segments.reserve(9);
            const double arrowBack = worldLength * 0.18;
            const double arrowSpread = worldLength * 0.07;
            for (const auto& basis : bases)
            {
                const glm::dvec3 tip = origin + basis.dir * worldLength;
                const glm::dvec3 shoulder = tip - basis.dir * arrowBack;
                emitWorldSegment(basis.axis, origin, tip);
                emitWorldSegment(basis.axis, tip, shoulder + basis.u * arrowSpread);
                emitWorldSegment(basis.axis, tip, shoulder - basis.u * arrowSpread);
            }
        }
        else if (mode == GizmoMode::Scale)
        {
            result.segments.reserve(15);
            const double halfBox = worldLength * 0.07;
            for (const auto& basis : bases)
            {
                const glm::dvec3 tip = origin + basis.dir * worldLength;
                emitWorldSegment(basis.axis, origin, tip);
                const std::array<glm::dvec3, 4> corners = {
                    tip + basis.u * halfBox + basis.v * halfBox,
                    tip - basis.u * halfBox + basis.v * halfBox,
                    tip - basis.u * halfBox - basis.v * halfBox,
                    tip + basis.u * halfBox - basis.v * halfBox,
                };
                for (size_t i = 0; i < 4; ++i)
                    emitWorldSegment(basis.axis, corners[i], corners[(i + 1) % 4]);
            }
        }
        else
        {
            result.segments.reserve(static_cast<size_t>(3 * ROTATE_RING_SEGMENTS));
            const double step = glm::two_pi<double>() / static_cast<double>(ROTATE_RING_SEGMENTS);
            for (const auto& basis : bases)
            {
                for (int i = 0; i < ROTATE_RING_SEGMENTS; ++i)
                {
                    const double a0 = step * static_cast<double>(i);
                    const double a1 = step * static_cast<double>((i + 1) % ROTATE_RING_SEGMENTS);
                    const glm::dvec3 p0 =
                        origin + (basis.u * std::cos(a0) + basis.v * std::sin(a0)) * worldLength;
                    const glm::dvec3 p1 =
                        origin + (basis.u * std::cos(a1) + basis.v * std::sin(a1)) * worldLength;
                    emitWorldSegment(basis.axis, p0, p1);
                }
            }
        }

        return result;
    }

    GizmoAxis pickGizmoAxis(glm::vec2 cursorPosition, const ProjectedGizmo& gizmo, float tolerance) noexcept
    {
        if (!gizmo.visible || !std::isfinite(cursorPosition.x) || !std::isfinite(cursorPosition.y) ||
            !std::isfinite(tolerance) || tolerance < 0.0f)
            return GizmoAxis::None;

        const glm::dvec2 cursor(cursorPosition);
        std::array<double, 3> minDistances = {
            std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity(),
        };

        for (const auto& item : gizmo.segments)
        {
            const size_t idx = axisIndex(item.axis);
            if (idx >= 3)
                continue;
            const auto& seg = item.segment;
            if (!std::isfinite(seg.start.x) || !std::isfinite(seg.start.y) || !std::isfinite(seg.end.x) ||
                !std::isfinite(seg.end.y))
                continue;
            const double dist = distancePointToSegment(cursor, glm::dvec2(seg.start), glm::dvec2(seg.end));
            if (dist < minDistances[idx])
                minDistances[idx] = dist;
        }

        const std::array<GizmoAxis, 3> order = {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z};
        double bestDist = static_cast<double>(tolerance);
        GizmoAxis bestAxis = GizmoAxis::None;
        for (size_t i = 0; i < 3; ++i)
        {
            const double dist = minDistances[i];
            if (!std::isfinite(dist) || dist > static_cast<double>(tolerance))
                continue;
            if (bestAxis == GizmoAxis::None || dist < bestDist)
            {
                bestDist = dist;
                bestAxis = order[i];
            }
        }
        return bestAxis;
    }
}  // namespace Incubator::Editor
