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
            std::vector<uint32_t> indices;
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
            // 每个面的起点是左下角；朝向 +Z 的顶面从 PNG 第一行读取数据。
            constexpr std::array<std::array<float, 2>, 4> UV = {
                {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}}};
            SceneMeshData data;
            data.vertices.reserve(24);
            data.indices.reserve(36);
            for (uint32_t face = 0; face < FACES.size(); ++face)
            {
                for (size_t corner = 0; corner < 4; ++corner)
                {
                    const auto& position = POSITIONS[FACES[face][corner]];
                    data.vertices.push_back({{position[0], position[1], position[2]}, {UV[corner][0], UV[corner][1]}});
                }
                const uint32_t vertex = face * 4;
                data.indices.insert(data.indices.end(),
                                    {vertex, vertex + 1, vertex + 2, vertex, vertex + 2, vertex + 3});
            }
            return data;
        }

        // 朝向 +Z 的单位 XY 平面，UV 原点约定与立方体相同，均位于左上角。
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
