#pragma once

#include <array>
#include <glm/vec4.hpp>

#include "runtime/core/math/aabb.h"
#include "runtime/render/frame_input.h"

namespace Incubator::Scene
{
    /// @brief 由最终 projection * view 矩阵生成的视锥体。
    /// @note 视锥体包含六个朝内平面，深度范围为零到一。视锥体不可用时保守地保留所有候选对象。
    class Frustum
    {
    public:
        explicit Frustum(const Render::CameraMatrices& camera) noexcept;

        /// @brief 判断包围盒是否可能与视锥体相交。
        /// @param bounds 待检查的包围盒。
        /// @return 可能相交时返回 true；接触平面或边界非有限时也会保留。
        bool intersects(const Math::Aabb& bounds) const noexcept;

    private:
        std::array<glm::dvec4, 6> planes_{};
        bool usable_ = false;
    };
}  // namespace Incubator::Scene
