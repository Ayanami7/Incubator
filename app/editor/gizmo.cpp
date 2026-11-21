#include "gizmo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <glm/ext/scalar_constants.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec4.hpp>
#include <limits>
#include <utility>

#include "app/common/viewport_input.h"
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

        std::optional<glm::dvec3> worldAxisVector(GizmoAxis axis) noexcept
        {
            switch (axis)
            {
                case GizmoAxis::X:
                    return glm::dvec3(1.0, 0.0, 0.0);
                case GizmoAxis::Y:
                    return glm::dvec3(0.0, 1.0, 0.0);
                case GizmoAxis::Z:
                    return glm::dvec3(0.0, 0.0, 1.0);
                case GizmoAxis::None:
                    break;
            }
            return std::nullopt;
        }

        struct ViewportRay
        {
            glm::dvec3 origin{0.0};
            glm::dvec3 direction{0.0, 0.0, -1.0};
        };

        std::optional<ViewportRay> unclippedViewportRay(const Scene::Camera& camera, Render::FrameExtent extent,
                                                        const App::ViewportRegion& region,
                                                        glm::vec2 cursorPosition) noexcept
        {
            if (!isValidViewport(extent, region) || !std::isfinite(cursorPosition.x) ||
                !std::isfinite(cursorPosition.y))
                return std::nullopt;

            const double nx =
                (static_cast<double>(cursorPosition.x) - static_cast<double>(region.origin.x)) / region.size.x;
            const double ny =
                (static_cast<double>(cursorPosition.y) - static_cast<double>(region.origin.y)) / region.size.y;
            const double ndcX = 2.0 * nx - 1.0;
            const double ndcY = 2.0 * ny - 1.0;
            if (!std::isfinite(ndcX) || !std::isfinite(ndcY))
                return std::nullopt;

            const auto matrices = camera.matrices(extent);
            const glm::dmat4 inverseVp = glm::inverse(glm::dmat4(matrices.projection) * glm::dmat4(matrices.view));
            const auto unproject = [&](double ndcZ) -> std::optional<glm::dvec3> {
                const glm::dvec4 h = inverseVp * glm::dvec4(ndcX, ndcY, ndcZ, 1.0);
                if (!std::isfinite(h.w) || h.w == 0.0)
                    return std::nullopt;
                const glm::dvec3 p = glm::dvec3(h) / h.w;
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                    return std::nullopt;
                return p;
            };

            const auto nearPoint = unproject(0.0);
            const auto farPoint = unproject(1.0);
            if (!nearPoint || !farPoint)
                return std::nullopt;

            const glm::dvec3 delta = *farPoint - *nearPoint;
            const double lenSq = glm::dot(delta, delta);
            const glm::dvec3 eye(camera.position());
            if (!std::isfinite(lenSq) || lenSq <= 1e-24 || !std::isfinite(eye.x) || !std::isfinite(eye.y) ||
                !std::isfinite(eye.z))
                return std::nullopt;

            return ViewportRay{eye, delta / std::sqrt(lenSq)};
        }

        std::optional<double> closestAxisParam(const ViewportRay& ray, const glm::dvec3& center,
                                               const glm::dvec3& axisDir) noexcept
        {
            const double b = glm::dot(axisDir, ray.direction);
            if (!std::isfinite(b) || std::abs(b) > 0.999)
                return std::nullopt;

            const double denom = 1.0 - b * b;
            if (denom <= 1e-6)
                return std::nullopt;

            const glm::dvec3 w0 = center - ray.origin;
            const double d1 = glm::dot(axisDir, w0);
            const double e = glm::dot(ray.direction, w0);
            const double t = (b * e - d1) / denom;
            const double s = (e - b * d1) / denom;
            if (!std::isfinite(t) || !std::isfinite(s) || s <= 0.0)
                return std::nullopt;
            return t;
        }

        std::optional<glm::dvec3> intersectAxisPlane(const ViewportRay& ray, const glm::dvec3& center,
                                                     const glm::dvec3& normal) noexcept
        {
            const double denom = glm::dot(ray.direction, normal);
            if (!std::isfinite(denom) || std::abs(denom) < 1e-4)
                return std::nullopt;

            const double s = glm::dot(center - ray.origin, normal) / denom;
            if (!std::isfinite(s) || s <= 0.0)
                return std::nullopt;

            const glm::dvec3 v = (ray.origin + ray.direction * s) - center;
            const double lenSq = glm::dot(v, v);
            if (!std::isfinite(lenSq) || lenSq < 1e-12)
                return std::nullopt;
            return v;
        }

        glm::dvec2 computeRotateScreenTangent(const glm::dvec3& center, const glm::dvec3& worldAxis,
                                              const glm::dvec3& rayDir, float worldLength,
                                              const glm::dmat4& viewProjection,
                                              const App::ViewportRegion& region) noexcept
        {
            glm::dvec3 worldTangent = glm::cross(worldAxis, -rayDir);
            double tangentLen = glm::length(worldTangent);
            if (!std::isfinite(tangentLen) || tangentLen < 1e-6)
            {
                const glm::dvec3 fallbackUp =
                    std::abs(worldAxis.y) < 0.9 ? glm::dvec3(0.0, 1.0, 0.0) : glm::dvec3(1.0, 0.0, 0.0);
                worldTangent = glm::cross(worldAxis, fallbackUp);
                tangentLen = glm::length(worldTangent);
            }
            if (!std::isfinite(tangentLen) || tangentLen < 1e-6)
                return glm::dvec2(1.0, 0.0);
            worldTangent /= tangentLen;

            const double step = std::max(static_cast<double>(worldLength) * 0.5, 1e-3);
            const auto p0 = clipToScreen(viewProjection * glm::dvec4(center, 1.0), region);
            const auto p1 = clipToScreen(viewProjection * glm::dvec4(center + worldTangent * step, 1.0), region);
            if (!p0 || !p1)
                return glm::dvec2(1.0, 0.0);

            const glm::dvec2 delta = glm::dvec2(*p1) - glm::dvec2(*p0);
            const double screenLen = glm::length(delta);
            if (!std::isfinite(screenLen) || screenLen < 1e-6)
                return glm::dvec2(1.0, 0.0);
            return delta / screenLen;
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

    GizmoDragState beginGizmoDrag(Scene::EntityId entity, GizmoMode mode, GizmoAxis axis,
                                  const Scene::Transform& startTransform, const Scene::Camera& camera,
                                  Render::FrameExtent extent, const App::ViewportRegion& region,
                                  glm::vec2 cursorPosition, float screenLength) noexcept
    {
        const auto worldAxisOpt = worldAxisVector(axis);
        if (!entity.valid() || !worldAxisOpt || !isValidViewport(extent, region) || !std::isfinite(cursorPosition.x) ||
            !std::isfinite(cursorPosition.y) || !std::isfinite(screenLength) || screenLength <= 0.0f)
            return {};

        const auto worldLengthOpt =
            computeGizmoWorldLength(startTransform.position(), camera, extent, region, screenLength);
        if (!worldLengthOpt)
            return {};

        const auto rayOpt = unclippedViewportRay(camera, extent, region, cursorPosition);
        if (!rayOpt)
            return {};

        GizmoDragState state;
        state.active = true;
        state.mode = mode;
        state.axis = axis;
        state.entity = entity;
        state.startTransform = startTransform;
        state.lastTransform = startTransform;
        state.startWorldLength = *worldLengthOpt;
        state.screenLength = screenLength;
        state.startCursor = glm::dvec2(cursorPosition);

        const glm::dvec3 center(startTransform.position());
        if (mode == GizmoMode::Translate)
        {
            state.dragAxis = *worldAxisOpt;
            if (const auto tOpt = closestAxisParam(*rayOpt, center, state.dragAxis))
            {
                state.startAxisParam = *tOpt;
                state.hasAxisReference = true;
            }
        }
        else if (mode == GizmoMode::Scale)
        {
            const glm::dquat q0 = glm::normalize(glm::dquat(startTransform.rotation()));
            state.dragAxis = glm::normalize(q0 * (*worldAxisOpt));
            if (const auto tOpt = closestAxisParam(*rayOpt, center, state.dragAxis))
            {
                state.startAxisParam = *tOpt;
                state.hasAxisReference = true;
            }
        }
        else
        {
            state.dragAxis = *worldAxisOpt;
            // 在起手帧一次性锁定平面交点或屏幕切线分支，拖拽途中不再切换以消除阈值跳变。
            state.planarMode = std::abs(glm::dot(rayOpt->direction, state.dragAxis)) >= 0.15;
            if (const auto v0 = intersectAxisPlane(*rayOpt, center, state.dragAxis))
            {
                state.startPlaneVector = glm::normalize(*v0);
                state.hasPlaneReference = true;
            }
            const auto matrices = camera.matrices(extent);
            const glm::dmat4 viewProjection = glm::dmat4(matrices.projection) * glm::dmat4(matrices.view);
            state.startScreenTangent = computeRotateScreenTangent(center, state.dragAxis, rayOpt->direction,
                                                                  state.startWorldLength, viewProjection, region);
        }

        return state;
    }

    Scene::Transform updateGizmoDrag(GizmoDragState& state, const Scene::Camera& camera, Render::FrameExtent extent,
                                     const App::ViewportRegion& region, glm::vec2 cursorPosition) noexcept
    {
        if (!state.active)
            return state.lastTransform;

        const auto rayOpt = unclippedViewportRay(camera, extent, region, cursorPosition);
        if (!rayOpt)
            return state.lastTransform;

        const glm::dvec3 center(state.startTransform.position());
        if (state.mode == GizmoMode::Translate)
        {
            const auto tOpt = closestAxisParam(*rayOpt, center, state.dragAxis);
            if (!tOpt)
                return state.lastTransform;
            if (!state.hasAxisReference)
            {
                state.startAxisParam = *tOpt;
                state.hasAxisReference = true;
                return state.lastTransform;
            }
            const glm::vec3 newPos(center + state.dragAxis * (*tOpt - state.startAxisParam));
            if (!isFiniteVec3(newPos))
                return state.lastTransform;
            state.lastTransform =
                Scene::Transform(newPos, state.startTransform.rotation(), state.startTransform.scale());
            return state.lastTransform;
        }

        if (state.mode == GizmoMode::Scale)
        {
            const size_t idx = axisIndex(state.axis);
            if (idx >= 3 || !std::isfinite(state.startWorldLength) || state.startWorldLength <= 0.0f)
                return state.lastTransform;
            const auto tOpt = closestAxisParam(*rayOpt, center, state.dragAxis);
            if (!tOpt)
                return state.lastTransform;
            if (!state.hasAxisReference)
            {
                state.startAxisParam = *tOpt;
                state.hasAxisReference = true;
                return state.lastTransform;
            }

            const double normalizedDelta =
                (*tOpt - state.startAxisParam) / static_cast<double>(state.startWorldLength);
            const glm::length_t comp = static_cast<glm::length_t>(idx);
            const double s0 = static_cast<double>(state.startTransform.scale()[comp]);
            double si = 0.0;
            if (std::abs(s0) >= 0.001)
            {
                si = s0 > 0.0 ? std::max(0.001, s0 * (1.0 + normalizedDelta))
                              : std::min(-0.001, s0 * (1.0 + normalizedDelta));
            }
            else
            {
                // 起始缩放接近 0 时以 1.0 为基准做加性缩放，避免 0 * (1 + delta) 恒为 0 导致卡死。
                si = std::max(0.001, s0 + normalizedDelta);
            }
            if (!std::isfinite(si))
                return state.lastTransform;

            glm::vec3 newScale = state.startTransform.scale();
            newScale[comp] = static_cast<float>(si);
            if (!isFiniteVec3(newScale))
                return state.lastTransform;
            state.lastTransform =
                Scene::Transform(state.startTransform.position(), state.startTransform.rotation(), newScale);
            return state.lastTransform;
        }

        double deltaTheta = 0.0;
        if (state.planarMode)
        {
            const auto v1Opt = intersectAxisPlane(*rayOpt, center, state.dragAxis);
            if (!v1Opt)
                return state.lastTransform;
            const glm::dvec3 v1 = glm::normalize(*v1Opt);
            if (!state.hasPlaneReference)
            {
                state.startPlaneVector = v1;
                state.hasPlaneReference = true;
                return state.lastTransform;
            }
            const double sinTerm = glm::dot(glm::cross(state.startPlaneVector, v1), state.dragAxis);
            const double cosTerm = glm::dot(state.startPlaneVector, v1);
            deltaTheta = std::atan2(sinTerm, cosTerm);
        }
        else
        {
            const glm::dvec2 deltaCursor = glm::dvec2(cursorPosition) - state.startCursor;
            const double projectedPx = glm::dot(deltaCursor, state.startScreenTangent);
            const double radiusPx = std::max(static_cast<double>(state.screenLength), 1.0);
            deltaTheta = projectedPx / radiusPx;
        }
        if (!std::isfinite(deltaTheta))
            return state.lastTransform;

        const glm::dquat q0 = glm::normalize(glm::dquat(state.startTransform.rotation()));
        const glm::dquat deltaQuat = glm::angleAxis(deltaTheta, state.dragAxis);
        const glm::quat rawRot(glm::normalize(deltaQuat * q0));
        if (!std::isfinite(rawRot.w) || !std::isfinite(rawRot.x) || !std::isfinite(rawRot.y) ||
            !std::isfinite(rawRot.z))
            return state.lastTransform;
        const float rotLenSq = glm::dot(rawRot, rawRot);
        if (!std::isfinite(rotLenSq) || rotLenSq <= 1e-12f)
            return state.lastTransform;

        const glm::quat newRot = glm::normalize(rawRot);
        state.lastTransform =
            Scene::Transform(state.startTransform.position(), newRot, state.startTransform.scale());
        return state.lastTransform;
    }
}  // namespace Incubator::Editor
