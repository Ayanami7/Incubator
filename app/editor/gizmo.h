#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <optional>
#include <vector>

#include "runtime/render/frame_input.h"
#include "runtime/scene/entity.h"
#include "runtime/scene/transform.h"
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

    /// @brief 单次 Gizmo 三轴拖拽交互的几何求解状态与锁定的起始基准。
    struct GizmoDragState
    {
        bool active = false;
        GizmoMode mode = GizmoMode::Translate;
        GizmoAxis axis = GizmoAxis::None;
        Scene::EntityId entity{};
        Scene::Transform startTransform{};
        Scene::Transform lastTransform{};
        float startWorldLength = 0.0f;
        float screenLength = 90.0f;
        glm::dvec3 dragAxis{0.0};
        double startAxisParam = 0.0;
        bool hasAxisReference = false;
        bool planarMode = false;
        glm::dvec3 startPlaneVector{0.0};
        bool hasPlaneReference = false;
        glm::dvec2 startCursor{0.0};
        glm::dvec2 startScreenTangent{1.0, 0.0};
        glm::dvec3 startWorldOrigin{0.0};
        glm::dmat4 inverseParentWorld{1.0};
    };

    /// @brief 在屏幕逻辑坐标空间中执行 Gizmo 三轴 2D 命中测试。
    /// @param cursorPosition 与投影图元处于同一坐标系（含 ViewportRegion::origin 偏移）的鼠标逻辑坐标。
    /// @param gizmo 当前帧投影得到的 Gizmo 图元。
    /// @param tolerance 命中判定容差（逻辑像素）。
    /// @return 距离小于等于容差的最短距离轴；距离完全相等时按 X -> Y -> Z 顺序返回；未命中返回 GizmoAxis::None。
    GizmoAxis pickGizmoAxis(glm::vec2 cursorPosition, const ProjectedGizmo& gizmo, float tolerance = 8.0f) noexcept;

    /// @brief 初始化 Gizmo 三轴拖拽状态并锁定起始几何与旋转求解分支。
    /// @param entity 被拖拽的选中实体标识。
    /// @param mode 本次拖拽锁定的 Gizmo 操作模式。
    /// @param axis 本次拖拽锁定的操作轴（需为 X / Y / Z）。
    /// @param startTransform 拖拽开始时刻实体的世界空间变换。
    /// @param camera 当前帧场景相机。
    /// @param extent 本帧实际渲染尺寸。
    /// @param region 本帧场景图像在 ImGui 中的逻辑区域。
    /// @param cursorPosition 与投影图元同坐标系（含 ViewportRegion::origin 偏移）的起始鼠标逻辑坐标。
    /// @param screenLength Gizmo 的屏幕逻辑参考长度（像素）。
    /// @return 初始化后的拖拽状态；参数无效或中心位于近裁剪面后方时 active 为 false。
    GizmoDragState beginGizmoDrag(Scene::EntityId entity, GizmoMode mode, GizmoAxis axis,
                                  const Scene::Transform& startTransform, const Scene::Camera& camera,
                                  Render::FrameExtent extent, const App::ViewportRegion& region,
                                  glm::vec2 cursorPosition, float screenLength = 90.0f,
                                  std::optional<glm::vec3> worldOrigin = std::nullopt,
                                  const glm::dmat4& inverseParentWorld = glm::dmat4(1.0)) noexcept;

    /// @brief 根据当前鼠标位置与相机求解 Gizmo 拖拽产生的新变换，支持鼠标跨出视口边界的非裁剪射线。
    /// @param state 可变拖拽状态，维护上一有效帧变换并在首帧缺失参考时补齐。
    /// @param camera 当前帧场景相机。
    /// @param extent 本帧实际渲染尺寸。
    /// @param region 本帧场景图像在 ImGui 中的逻辑区域。
    /// @param cursorPosition 与投影图元同坐标系（含 ViewportRegion::origin 偏移）的当前鼠标逻辑坐标。
    /// @return 更新后的实体变换；当视线退化、平行或数值非有限时保持上一有效帧变换不变。
    Scene::Transform updateGizmoDrag(GizmoDragState& state, const Scene::Camera& camera, Render::FrameExtent extent,
                                     const App::ViewportRegion& region, glm::vec2 cursorPosition) noexcept;
}  // namespace Incubator::Editor
