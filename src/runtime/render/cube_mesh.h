#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Incubator
{
    namespace Render
    {
        namespace Detail
        {
            struct CubeVertex
            {
                float position[3];
                float uv[2];
            };
            static_assert(sizeof(CubeVertex) == 5 * sizeof(float));

            constexpr std::array<CubeVertex, 24> makeCubeVertices()
            {
                constexpr std::array<std::array<float, 3>, 8> positions = {{{-0.5f, -0.5f, -0.5f},
                                                                            {0.5f, -0.5f, -0.5f},
                                                                            {0.5f, 0.5f, -0.5f},
                                                                            {-0.5f, 0.5f, -0.5f},
                                                                            {-0.5f, -0.5f, 0.5f},
                                                                            {0.5f, -0.5f, 0.5f},
                                                                            {0.5f, 0.5f, 0.5f},
                                                                            {-0.5f, 0.5f, 0.5f}}};
                constexpr std::array<std::array<uint8_t, 4>, 6> faces = {
                    {{4, 5, 6, 7}, {1, 0, 3, 2}, {5, 1, 2, 6}, {0, 4, 7, 3}, {7, 6, 2, 3}, {0, 1, 5, 4}}};
                // Each face starts at its bottom-left corner; +Z top maps to PNG row zero.
                constexpr std::array<std::array<float, 2>, 4> uv = {
                    {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}}};
                std::array<CubeVertex, 24> vertices{};
                for (size_t face = 0; face < faces.size(); ++face)
                {
                    for (size_t corner = 0; corner < 4; ++corner)
                    {
                        const auto& position = positions[faces[face][corner]];
                        vertices[face * 4 + corner] = {{position[0], position[1], position[2]},
                                                       {uv[corner][0], uv[corner][1]}};
                    }
                }
                return vertices;
            }

            constexpr std::array<uint16_t, 36> makeCubeIndices()
            {
                std::array<uint16_t, 36> indices{};
                for (uint16_t face = 0; face < 6; ++face)
                {
                    const uint16_t vertex = face * 4;
                    const size_t index = face * 6;
                    indices[index] = vertex;
                    indices[index + 1] = vertex + 1;
                    indices[index + 2] = vertex + 2;
                    indices[index + 3] = vertex;
                    indices[index + 4] = vertex + 2;
                    indices[index + 5] = vertex + 3;
                }
                return indices;
            }

            inline constexpr auto CUBE_VERTICES = makeCubeVertices();
            inline constexpr auto CUBE_INDICES = makeCubeIndices();
        }  // namespace Detail
    }  // namespace Render
}  // namespace Incubator
