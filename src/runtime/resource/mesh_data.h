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

        // CPU geometry only. ModelResource validates these arrays when taking ownership.
        struct MeshData
        {
            static constexpr size_t MAX_VERTEX_COUNT = 65536;

            std::vector<MeshVertexData> vertices;
            std::vector<uint16_t> indices;
        };
    }  // namespace Resource
}  // namespace Incubator
