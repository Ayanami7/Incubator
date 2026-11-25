#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/vec3.hpp>
#include <vector>

namespace Incubator::Render
{
    /// @brief 单帧最多支持的局部光源数量上限（点光源与聚光灯共用）。
    inline constexpr size_t MAX_LOCAL_LIGHTS = 16;

    /// @brief 场景局部光源类型。
    enum class LocalLightType : uint32_t
    {
        Point = 0,  ///< 全向点光源。
        Spot = 1    ///< 带内外锥角衰减的聚光灯。
    };

    /// @brief 世界空间点光源或聚光灯参数。
    struct LocalLight
    {
        LocalLightType type = LocalLightType::Point;
        bool enabled = true;
        glm::vec3 position{0.0f, 2.0f, 0.0f};   ///< 世界空间光源位置，须为有限值。
        glm::vec3 direction{0.0f, -1.0f, 0.0f}; ///< 聚光灯光线传播方向，有限且非零；允许非单位输入。
        glm::vec3 color{1.0f, 1.0f, 1.0f};      ///< 有限的线性 RGB，各分量位于 [0, 1]。
        float intensity = 5.0f;                 ///< 有限非负辐射强度。
        float range = 10.0f;                    ///< 有限正衰减半径（> 0）。
        float innerConeAngle = 0.35f;           ///< 聚光灯内半角（弧度），位于 [0, outerConeAngle]。
        float outerConeAngle = 0.6f;            ///< 聚光灯外半角（弧度），位于 [innerConeAngle, pi/2)。

        /// @brief 检查局部光源位置、方向、颜色、强度、范围与锥角是否有效。
        bool valid() const noexcept;
        bool operator==(const LocalLight&) const = default;
    };

    /// @brief 一帧共享的世界空间方向光、环境项及局部光源列表。
    struct Lighting
    {
        glm::vec3 direction{0, 0, -1};  ///< 光线传播方向，有限且非零；允许非单位输入。
        glm::vec3 color{1, 1, 1};       ///< 有限的线性 RGB，各分量位于 [0, 1]。
        float intensity = 1.0f;         ///< 有限非负强度，零表示关闭直接光。
        float ambient = 0.1f;           ///< 有限的标量环境系数，位于 [0, 1]。
        std::vector<LocalLight> localLights{};  ///< 可选的局部光源列表，总数不超过 MAX_LOCAL_LIGHTS。

        /// @brief 检查方向光、环境系数与所有局部光源是否满足输入约定。
        bool valid() const noexcept;
        bool operator==(const Lighting&) const = default;
    };
}  // namespace Incubator::Render
