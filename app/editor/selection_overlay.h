#pragma once

#include <array>
#include <cmath>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <optional>
#include <utility>
#include <vector>

#include "app/common/ui_input.h"
#include "runtime/model/prepared_model.h"
#include "runtime/render/frame_input.h"
#include "runtime/scene/scene.h"

namespace Incubator::Editor
{
    /// @brief 视口图像逻辑坐标空间中的二维辅助线段。
    struct OverlaySegment
    {
        glm::vec2 start;
        glm::vec2 end;
    };

    /// @brief 在 Vulkan 齐次裁剪空间（X/Y 位于 [-w, w]，Z 位于 [0, w]）中裁剪线段。
    /// @param start 齐次裁剪空间起点。
    /// @param end 齐次裁剪空间终点。
    /// @return 裁剪后的齐次线段端点（保证两端 w > 0 且有限）；完全不可见或输入非法时返回空值。
    inline std::optional<std::pair<glm::dvec4, glm::dvec4>> clipHomogeneousSegment(const glm::dvec4& start,
                                                                                   const glm::dvec4& end) noexcept
    {
        auto finite = [](const glm::dvec4& value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
        };
        if (!finite(start) || !finite(end))
            return std::nullopt;

        const std::array<glm::dvec4, 6> planes = {
            glm::dvec4(1.0, 0.0, 0.0, 1.0),   // x + w >= 0
            glm::dvec4(-1.0, 0.0, 0.0, 1.0),  // w - x >= 0
            glm::dvec4(0.0, 1.0, 0.0, 1.0),   // y + w >= 0
            glm::dvec4(0.0, -1.0, 0.0, 1.0),  // w - y >= 0
            glm::dvec4(0.0, 0.0, 1.0, 0.0),   // z >= 0
            glm::dvec4(0.0, 0.0, -1.0, 1.0),  // w - z >= 0
        };

        double tMin = 0.0;
        double tMax = 1.0;
        for (const auto& plane : planes)
        {
            const double d0 = glm::dot(plane, start);
            const double d1 = glm::dot(plane, end);
            if (d0 < 0.0 && d1 < 0.0)
                return std::nullopt;
            if (d0 < 0.0 || d1 < 0.0)
            {
                const double denom = d0 - d1;
                if (denom == 0.0 || !std::isfinite(denom))
                    return std::nullopt;
                const double t = d0 / denom;
                if (!std::isfinite(t))
                    return std::nullopt;
                if (d0 < 0.0)
                    tMin = std::max(tMin, t);
                else
                    tMax = std::min(tMax, t);
                if (tMin > tMax)
                    return std::nullopt;
            }
        }

        const glm::dvec4 clippedStart = start + (end - start) * tMin;
        const glm::dvec4 clippedEnd = start + (end - start) * tMax;
        if (!finite(clippedStart) || !finite(clippedEnd) || clippedStart.w <= 0.0 || clippedEnd.w <= 0.0)
            return std::nullopt;
        return std::pair{clippedStart, clippedEnd};
    }

    /// @brief 将世界空间 AABB 的十二条边经当前相机投影并裁剪到视口图像逻辑坐标。
    /// @param worldBounds 世界空间轴对齐包围盒。
    /// @param camera 本帧相机状态。
    /// @param extent 本帧实际场景渲染尺寸。
    /// @param region 本帧场景图像逻辑区域。
    /// @return 可见裁剪后的二维辅助线段集合；视口无效或包围盒全不可见时为空。
    inline std::vector<OverlaySegment> projectAabbOverlay(const Math::Aabb& worldBounds, const Scene::Camera& camera,
                                                          Render::FrameExtent extent,
                                                          const App::ViewportRegion& region)
    {
        if (!extent.width || !extent.height || !std::isfinite(region.origin.x) || !std::isfinite(region.origin.y) ||
            !std::isfinite(region.size.x) || !std::isfinite(region.size.y) || region.size.x <= 0.0 ||
            region.size.y <= 0.0)
            return {};

        const auto matrices = camera.matrices(extent);
        const glm::dmat4 viewProjection = glm::dmat4(matrices.projection) * glm::dmat4(matrices.view);

        std::array<glm::dvec4, 8> corners{};
        for (int i = 0; i < 8; ++i)
        {
            const glm::dvec4 point((i & 1) ? worldBounds.max.x : worldBounds.min.x,
                                   (i & 2) ? worldBounds.max.y : worldBounds.min.y,
                                   (i & 4) ? worldBounds.max.z : worldBounds.min.z, 1.0);
            corners[static_cast<size_t>(i)] = viewProjection * point;
        }

        static constexpr std::array<std::pair<int, int>, 12> EDGES = {{
            {0, 1},
            {2, 3},
            {4, 5},
            {6, 7},
            {0, 2},
            {1, 3},
            {4, 6},
            {5, 7},
            {0, 4},
            {1, 5},
            {2, 6},
            {3, 7},
        }};

        auto toScreen = [&](const glm::dvec4& clip) -> std::optional<glm::vec2> {
            const double ndcX = clip.x / clip.w;
            const double ndcY = clip.y / clip.w;
            if (!std::isfinite(ndcX) || !std::isfinite(ndcY))
                return std::nullopt;
            // Camera::matrices 已应用一次 Vulkan Y 轴适配，此处沿用 [-1, 1] -> [0, 1] 逻辑映射，不重复翻 Y。
            const double sx = region.origin.x + (ndcX + 1.0) * 0.5 * region.size.x;
            const double sy = region.origin.y + (ndcY + 1.0) * 0.5 * region.size.y;
            if (!std::isfinite(sx) || !std::isfinite(sy))
                return std::nullopt;
            return glm::vec2(static_cast<float>(sx), static_cast<float>(sy));
        };

        std::vector<OverlaySegment> segments;
        segments.reserve(EDGES.size());
        for (const auto& [a, b] : EDGES)
        {
            const auto clipped =
                clipHomogeneousSegment(corners[static_cast<size_t>(a)], corners[static_cast<size_t>(b)]);
            if (!clipped)
                continue;
            const auto p0 = toScreen(clipped->first);
            const auto p1 = toScreen(clipped->second);
            if (!p0 || !p1)
                continue;
            segments.push_back({*p0, *p1});
        }
        return segments;
    }

    /// @brief 将选中实体的世界 AABB 十二条边投影并裁剪到视口逻辑坐标。
    /// @param scene 当前场景只读引用，使用本帧编辑后的相机与实体变换。
    /// @param selected 唯一选中实体标识。
    /// @param extent 本帧实际场景渲染尺寸。
    /// @param region 本帧场景图像逻辑区域。
    /// @return 可见裁剪后的二维辅助线段集合；无有效选中、隐藏、无边界或视口无效时为空。
    inline std::vector<OverlaySegment> projectSelectionBox(const Scene::Scene& scene, Scene::EntityId selected,
                                                           Render::FrameExtent extent,
                                                           const App::ViewportRegion& region)
    {
        const auto* entity = scene.find(selected);
        if (!entity || !entity->visible || !entity->model || !entity->model->bounds())
            return {};
        const auto worldBounds = entity->model->bounds()->transformed(entity->transform.matrix());
        if (!worldBounds)
            return {};
        return projectAabbOverlay(*worldBounds, scene.camera(), extent, region);
    }
}  // namespace Incubator::Editor
