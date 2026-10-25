#pragma once

#include <glm/vec3.hpp>

namespace Incubator::Render
{
    /// @brief 一帧共享的世界空间方向光与环境项。
    struct Lighting
    {
        glm::vec3 direction{0, 0, -1};  ///< 光线传播方向，有限且非零；允许非单位输入。
        glm::vec3 color{1, 1, 1};       ///< 有限的线性 RGB，各分量位于 [0, 1]。
        float intensity = 1.0f;         ///< 有限非负强度，零表示关闭直接光。
        float ambient = 0.1f;           ///< 有限的标量环境系数，位于 [0, 1]。

        /// @brief 检查方向、颜色、强度与环境系数是否满足输入约定。
        bool valid() const noexcept;
        bool operator==(const Lighting&) const = default;
    };
}  // namespace Incubator::Render
