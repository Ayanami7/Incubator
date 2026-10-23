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
