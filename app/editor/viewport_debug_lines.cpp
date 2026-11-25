#include "viewport_debug_lines.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include "runtime/model/prepared_model.h"
#include "runtime/scene/scene.h"

namespace Incubator::Editor
{
    namespace
    {
        void appendCircle(std::vector<Render::DebugLine>& out, glm::vec3 center, glm::vec3 axisA, glm::vec3 axisB,
                          float radius, int segments, glm::vec3 color, float alpha)
        {
            const float step = glm::two_pi<float>() / static_cast<float>(segments);
            glm::vec3 prev = center + axisA * radius;
            for (int i = 1; i <= segments; ++i)
            {
                const float angle = step * static_cast<float>(i);
                const glm::vec3 next = center + (axisA * std::cos(angle) + axisB * std::sin(angle)) * radius;
                out.push_back({prev, next, color, alpha});
                prev = next;
            }
        }

        void buildBasis(glm::vec3 forward, glm::vec3& right, glm::vec3& up)
        {
            const glm::vec3 fallbackUp = std::abs(forward.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                                     : glm::vec3(1.0f, 0.0f, 0.0f);
            right = glm::normalize(glm::cross(fallbackUp, forward));
            up = glm::normalize(glm::cross(forward, right));
        }

        void appendAabbWireframe(std::vector<Render::DebugLine>& out, const Math::Aabb& bounds, glm::vec3 color,
                                 float alpha)
        {
            const glm::vec3& bMin = bounds.min;
            const glm::vec3& bMax = bounds.max;
            const std::array<glm::vec3, 8> corners{{
                {bMin.x, bMin.y, bMin.z},
                {bMax.x, bMin.y, bMin.z},
                {bMax.x, bMax.y, bMin.z},
                {bMin.x, bMax.y, bMin.z},
                {bMin.x, bMin.y, bMax.z},
                {bMax.x, bMin.y, bMax.z},
                {bMax.x, bMax.y, bMax.z},
                {bMin.x, bMax.y, bMax.z},
            }};
            constexpr std::array<std::pair<int, int>, 12> edges{{
                {0, 1}, {1, 2}, {2, 3}, {3, 0},  // -Z 底面
                {4, 5}, {5, 6}, {6, 7}, {7, 4},  // +Z 顶面
                {0, 4}, {1, 5}, {2, 6}, {3, 7},  // 纵向四棱
            }};
            for (const auto& [a, b] : edges)
                out.push_back({corners[a], corners[b], color, alpha});
        }
    }  // namespace

    std::vector<Render::DebugLine> buildViewportDebugLines(const Scene::Scene& scene, Scene::EntityId selected,
                                                           const ViewportDebugOptions& options)
    {
        std::vector<Render::DebugLine> lines;
        if (options.showGrid)
        {
            constexpr int HALF_EXTENT = 10;
            constexpr float BOUND = static_cast<float>(HALF_EXTENT);
            constexpr glm::vec3 GRID_COLOR{0.38f, 0.38f, 0.40f};
            constexpr glm::vec3 X_AXIS_COLOR{0.85f, 0.28f, 0.28f};
            constexpr glm::vec3 Z_AXIS_COLOR{0.28f, 0.48f, 0.85f};
            for (int i = -HALF_EXTENT; i <= HALF_EXTENT; ++i)
            {
                const float coord = static_cast<float>(i);
                const bool isAxis = (i == 0);
                // 沿 X 轴方向线段（其中 z=0 为 X 主轴）。
                lines.push_back({{-BOUND, 0.0f, coord},
                                 {BOUND, 0.0f, coord},
                                 isAxis ? X_AXIS_COLOR : GRID_COLOR,
                                 isAxis ? 0.72f : 0.38f});
                // 沿 Z 轴方向线段（其中 x=0 为 Z 主轴）。
                lines.push_back({{coord, 0.0f, -BOUND},
                                 {coord, 0.0f, BOUND},
                                 isAxis ? Z_AXIS_COLOR : GRID_COLOR,
                                 isAxis ? 0.72f : 0.38f});
            }
        }

        if (options.showSelectionBounds && selected.valid())
        {
            if (const auto* entity = scene.find(selected); entity && entity->model && entity->model->bounds())
            {
                if (const auto world = scene.worldMatrix(selected))
                {
                    if (const auto bounds = entity->model->bounds()->transformed(*world))
                        appendAabbWireframe(lines, *bounds, {0.92f, 0.70f, 0.18f}, 0.90f);
                }
            }
        }

        if (options.showLightGizmos)
        {
            const auto& lighting = scene.lighting();
            const float dirLen = glm::length(lighting.direction);
            if (dirLen > 1e-6f)
            {
                const glm::vec3 dir = lighting.direction / dirLen;
                glm::vec3 right{};
                glm::vec3 up{};
                buildBasis(dir, right, up);
                const glm::vec3 anchor{0.0f, 3.5f, 0.0f};
                const glm::vec3 tip = anchor + dir * 1.4f;
                const glm::vec3 dirColor{0.92f, 0.82f, 0.32f};
                lines.push_back({anchor, tip, dirColor, 0.85f});
                lines.push_back({tip, tip - dir * 0.3f + right * 0.16f, dirColor, 0.85f});
                lines.push_back({tip, tip - dir * 0.3f - right * 0.16f, dirColor, 0.85f});
            }

            constexpr int CIRCLE_SEGMENTS = 12;
            constexpr int SPOT_RING_SEGMENTS = 16;
            for (const auto& light : lighting.localLights)
            {
                const glm::vec3 color = light.enabled ? glm::clamp(light.color, glm::vec3(0.0f), glm::vec3(0.90f))
                                                      : glm::vec3(0.42f, 0.42f, 0.45f);
                const float alpha = light.enabled ? 0.88f : 0.45f;
                if (light.type == Render::LocalLightType::Point)
                {
                    constexpr float MARKER_RADIUS = 0.35f;
                    appendCircle(lines, light.position, {1, 0, 0}, {0, 1, 0}, MARKER_RADIUS, CIRCLE_SEGMENTS, color,
                                 alpha);
                    appendCircle(lines, light.position, {1, 0, 0}, {0, 0, 1}, MARKER_RADIUS, CIRCLE_SEGMENTS, color,
                                 alpha);
                    appendCircle(lines, light.position, {0, 1, 0}, {0, 0, 1}, MARKER_RADIUS, CIRCLE_SEGMENTS, color,
                                 alpha);
                }
                else
                {
                    const float len = glm::length(light.direction);
                    if (len <= 1e-6f)
                        continue;
                    const glm::vec3 forward = light.direction / len;
                    glm::vec3 right{};
                    glm::vec3 up{};
                    buildBasis(forward, right, up);
                    const float coneLen = std::min(light.range, 3.0f);
                    const float coneRadius = coneLen * std::tan(light.outerConeAngle);
                    const glm::vec3 baseCenter = light.position + forward * coneLen;
                    lines.push_back({light.position, baseCenter, color, alpha});
                    for (const glm::vec3 offset : {right * coneRadius, -right * coneRadius, up * coneRadius,
                                                   -up * coneRadius})
                        lines.push_back({light.position, baseCenter + offset, color, alpha});
                    appendCircle(lines, baseCenter, right, up, coneRadius, SPOT_RING_SEGMENTS, color, alpha);
                }
            }
        }
        return lines;
    }
}  // namespace Incubator::Editor
