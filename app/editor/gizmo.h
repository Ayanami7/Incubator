#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <optional>
#include <vector>

#include "runtime/render/frame_input.h"
#include "runtime/scene/entity.h"
#include "selection_overlay.h"

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
    /// @brief Transform Gizmo 的操作模式。
    enum class GizmoMode
    {
        Translate,
        Rotate,
        Scale
    };

    /// @brief Transform Gizmo 的坐标轴标识。
    enum class GizmoAxis
    {
        None,
        X,
        Y,
        Z
    };

    /// @brief 投影到视口逻辑空间中的单条 Gizmo 辅助线段。
    struct GizmoSegment
    {
        GizmoAxis axis = GizmoAxis::None;
        OverlaySegment segment{};
    };

    /// @brief 当前帧 Gizmo 的投影几何与状态摘要。
    struct ProjectedGizmo
    {
        std::vector<GizmoSegment> segments;
        glm::vec3 origin{0.0f};
        float worldLength = 0.0f;
        bool visible = false;
    };

    /// @brief 计算维持 Gizmo 屏幕恒定逻辑像素尺度的世界空间轴长。
    /// @param center Gizmo 在世界空间中的原点位置。
    /// @param camera 当前场景相机。
    /// @param extent 本帧实际渲染尺寸。
    /// @param region 本帧场景图像在 ImGui 中的逻辑区域。
    /// @param screenLength 期望的屏幕逻辑参考长度（像素）。
    /// @return 世界空间轴长；中心位于近裁剪面后方或视口参数无效时返回空值。
    std::optional<float> computeGizmoWorldLength(const glm::vec3& center, const Scene::Camera& camera,
                                                 Render::FrameExtent extent, const App::ViewportRegion& region,
                                                 float screenLength = 90.0f) noexcept;

    /// @brief 构造选中实体的 3D Gizmo 几何并执行齐次裁剪投影到视口逻辑坐标。
    /// @param scene 当前场景只读引用，使用本帧最新相机与实体变换。
    /// @param selected 当前唯一选中实体标识。
    /// @param mode Gizmo 操作模式（Translate / Rotate / Scale）。
    /// @param extent 本帧实际渲染尺寸。
    /// @param region 本帧场景图像在 ImGui 中的逻辑区域。
    /// @param screenLength 期望的屏幕逻辑参考长度（像素）。
    /// @return 投影结果；无有效选中、实体不可见、中心位于近裁剪面后方或视口无效时 visible 为 false 且 segments 为空。
    ProjectedGizmo projectGizmo(const Scene::Scene& scene, Scene::EntityId selected, GizmoMode mode,
                                Render::FrameExtent extent, const App::ViewportRegion& region,
                                float screenLength = 90.0f);

    /// @brief 在屏幕逻辑坐标空间中执行 Gizmo 三轴 2D 命中测试。
    /// @param cursorPosition 与投影图元处于同一坐标系（含 ViewportRegion::origin 偏移）的鼠标逻辑坐标。
    /// @param gizmo 当前帧投影得到的 Gizmo 图元。
    /// @param tolerance 命中判定容差（逻辑像素）。
    /// @return 距离小于等于容差的最短距离轴；距离完全相等时按 X -> Y -> Z 顺序返回；未命中返回 GizmoAxis::None。
    GizmoAxis pickGizmoAxis(glm::vec2 cursorPosition, const ProjectedGizmo& gizmo, float tolerance = 8.0f) noexcept;
}  // namespace Incubator::Editor
