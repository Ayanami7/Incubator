#pragma once

#include <vector>

#include "runtime/render/frame_input.h"
#include "runtime/scene/entity.h"

namespace Incubator::Scene
{
    class Scene;
}

namespace Incubator::Editor
{
    /// @brief 编辑器视口 3D 调试辅助线显示开关（仅作用于当前会话，不写入场景文件）。
    struct ViewportDebugOptions
    {
        bool showGrid = true;             ///< XZ 地面参考网格与主轴指示。
        bool showSelectionBounds = true;  ///< 选中实体世界空间 AABB 线框。
        bool showLightGizmos = true;      ///< 方向光与局部光源（点光/聚光）3D 指示线。

        bool operator==(const ViewportDebugOptions&) const = default;
    };

    /// @brief 根据当前场景实体、选中标识与显示开关构建世界空间 3D 调试线段列表（零 ImGui 依赖）。
    std::vector<Render::DebugLine> buildViewportDebugLines(const Scene::Scene& scene, Scene::EntityId selected,
                                                           const ViewportDebugOptions& options);
}  // namespace Incubator::Editor
