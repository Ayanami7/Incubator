#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "runtime/render/mesh.h"

namespace Incubator
{
    namespace Demo
    {
        struct SceneMeshData
        {
            std::vector<Render::MeshVertex> vertices;
            std::vector<uint16_t> indices;
        };

        inline SceneMeshData makeCubeMesh()
        {
            constexpr std::array<std::array<float, 3>, 8> POSITIONS = {{{-0.5f, -0.5f, -0.5f},
                                                                        {0.5f, -0.5f, -0.5f},
                                                                        {0.5f, 0.5f, -0.5f},
                                                                        {-0.5f, 0.5f, -0.5f},
                                                                        {-0.5f, -0.5f, 0.5f},
                                                                        {0.5f, -0.5f, 0.5f},
                                                                        {0.5f, 0.5f, 0.5f},
                                                                        {-0.5f, 0.5f, 0.5f}}};
            constexpr std::array<std::array<uint8_t, 4>, 6> FACES = {
                {{4, 5, 6, 7}, {1, 0, 3, 2}, {5, 1, 2, 6}, {0, 4, 7, 3}, {7, 6, 2, 3}, {0, 1, 5, 4}}};
            // Each face starts at its bottom-left; +Z top reads the first PNG row.
            constexpr std::array<std::array<float, 2>, 4> UV = {
                {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}}};
            SceneMeshData data;
            data.vertices.reserve(24);
            data.indices.reserve(36);
            for (uint16_t face = 0; face < FACES.size(); ++face)
            {
                for (size_t corner = 0; corner < 4; ++corner)
                {
                    const auto& position = POSITIONS[FACES[face][corner]];
                    data.vertices.push_back({{position[0], position[1], position[2]}, {UV[corner][0], UV[corner][1]}});
                }
                const uint16_t vertex = face * 4;
                data.indices.insert(data.indices.end(),
                                    {vertex, static_cast<uint16_t>(vertex + 1), static_cast<uint16_t>(vertex + 2),
                                     vertex, static_cast<uint16_t>(vertex + 2), static_cast<uint16_t>(vertex + 3)});
            }
            return data;
        }

        // Unit XY plane facing +Z, with the same top-left UV convention as the cube.
        inline SceneMeshData makePlaneMesh()
        {
            return {{{{-0.5f, -0.5f, 0.0f}, {0.0f, 1.0f}},
                     {{0.5f, -0.5f, 0.0f}, {1.0f, 1.0f}},
                     {{0.5f, 0.5f, 0.0f}, {1.0f, 0.0f}},
                     {{-0.5f, 0.5f, 0.0f}, {0.0f, 0.0f}}},
                    {0, 1, 2, 0, 2, 3}};
        }
    }  // namespace Demo
}  // namespace Incubator
