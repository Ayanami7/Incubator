#pragma once

#include <cstdint>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <optional>
#include <utility>
#include <vector>

#include "runtime/core/math/aabb.h"
#include "runtime/render/frame_input.h"
#include "runtime/scene/entity.h"

namespace Incubator::App
{
    struct ViewportRegion;
}

namespace Incubator::Scene
{
    class Camera;
    class Scene;
}  // namespace Incubator::Scene

namespace Incubator::Editor
{
    /// @brief 视口图像逻辑坐标空间中的二维辅助线段。
    struct OverlaySegment
    {
        glm::vec2 start;
        glm::vec2 end;
    };

    /// @brief 纯 C++ 叠加图元 RGBA 颜色（每通道 0–255）。
    struct OverlayColor
    {
        uint8_t r = 255;
        uint8_t g = 255;
        uint8_t b = 255;
        uint8_t a = 255;
    };

    /// @brief 视口叠加层中的单条带颜色与线宽的辅助线段。
    struct OverlayDrawLine
    {
        OverlaySegment segment{};
        OverlayColor color{};
        float thickness = 1.0f;
    };

    /// @brief 纯 C++ 视口叠加几何图元批次（包含裁剪矩形与按绘制顺序排列的线段列表）。
    struct ViewportOverlay
    {
        glm::vec2 clipMin{0.0f};
        glm::vec2 clipMax{0.0f};
        std::vector<OverlayDrawLine> lines;
    };

    /// @brief 在 Vulkan 齐次裁剪空间（X/Y 位于 [-w, w]，Z 位于 [0, w]）中裁剪线段。
    /// @param start 齐次裁剪空间起点。
    /// @param end 齐次裁剪空间终点。
    /// @return 裁剪后的齐次线段端点（保证两端 w > 0 且有限）；完全不可见或输入非法时返回空值。
    std::optional<std::pair<glm::dvec4, glm::dvec4>> clipHomogeneousSegment(const glm::dvec4& start,
                                                                            const glm::dvec4& end) noexcept;

    /// @brief 将世界空间 AABB 的十二条边经当前相机投影并裁剪到视口图像逻辑坐标。
    /// @param worldBounds 世界空间轴对齐包围盒。
    /// @param camera 本帧相机状态。
    /// @param extent 本帧实际场景渲染尺寸。
    /// @param region 本帧场景图像逻辑区域。
    /// @return 可见裁剪后的二维辅助线段集合；视口无效或包围盒全不可见时为空。
    std::vector<OverlaySegment> projectAabbOverlay(const Math::Aabb& worldBounds, const Scene::Camera& camera,
                                                   Render::FrameExtent extent, const App::ViewportRegion& region);

    /// @brief 将选中实体的世界 AABB 十二条边投影并裁剪到视口逻辑坐标。
    /// @param scene 当前场景只读引用，使用本帧编辑后的相机与实体变换。
    /// @param selected 唯一选中实体标识。
    /// @param extent 本帧实际场景渲染尺寸。
    /// @param region 本帧场景图像逻辑区域。
    /// @return 可见裁剪后的二维辅助线段集合；无有效选中、隐藏、无边界或视口无效时为空。
    std::vector<OverlaySegment> projectSelectionBox(const Scene::Scene& scene, Scene::EntityId selected,
                                                    Render::FrameExtent extent, const App::ViewportRegion& region);
}  // namespace Incubator::Editor
