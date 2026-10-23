#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <optional>

namespace Incubator
{
    namespace Math
    {
        /// @brief 由有限且有序的最小、最大端点表示的包围盒。
        /// @note 端点相等时仍有效；空集合应使用 optional<Aabb> 表示。
        struct Aabb
        {
            glm::vec3 min;
            glm::vec3 max;

            void include(const glm::vec3& point) noexcept;
            void merge(const Aabb& other) noexcept;
            bool contains(const glm::vec3& point) const noexcept;

            /// @brief 使用矩阵变换包围盒。
            /// @param matrix 用于变换包围盒的矩阵。
            /// @return 变换后的包围盒；无法计算时返回 std::nullopt。
            /// @note 不执行透视除法。无法取得边界时必须保留原有绘制路径。
            std::optional<Aabb> transformed(const glm::mat4& matrix) const noexcept;
        };
    }  // namespace Math
}  // namespace Incubator
