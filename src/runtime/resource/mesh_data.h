#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Incubator
{
    namespace Resource
    {
        struct MeshVertexData
        {
            std::array<float, 3> position{};
            std::array<float, 2> uv{};
            std::array<float, 3> normal{};    ///< 几何局部法线；零向量表示无可用法线。
            std::array<float, 4> tangent{};   ///< 局部切线（xyz）与副切线手性符号（w = ±1）；零向量或 w = 0 表示无可用切线。
        };

        /// @brief 由 CPU 持有的网格顶点和索引数据。
        /// @note ModelResource 接管这些数组时会进行验证。
        struct MeshData
        {
            std::vector<MeshVertexData> vertices;
            std::vector<uint32_t> indices;
        };
    }  // namespace Resource
}  // namespace Incubator
