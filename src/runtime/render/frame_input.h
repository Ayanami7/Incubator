#pragma once

#include <cstdint>
#include <glm/mat4x4.hpp>
#include <span>

#include "runtime/render/material.h"
#include "runtime/render/mesh.h"

namespace Incubator
{
    namespace Render
    {
        struct FrameExtent
        {
            uint32_t width;
            uint32_t height;

            bool operator==(const FrameExtent&) const = default;
        };

        struct CameraMatrices
        {
            glm::mat4 view;
            /// @brief 最终右手系、零到一深度投影矩阵，包含应用所需的 Y 轴适配。
            glm::mat4 projection;
        };

        struct DrawItem
        {
            Mesh mesh;
            Material material;
            glm::mat4 model;
        };

        /// @brief 提交一帧渲染所需的应用输入。
        /// @note 绘制列表仅在 drawFrame 调用期间借用，也可以为空。
        struct FrameInput
        {
            FrameExtent extent;
            CameraMatrices camera;
            std::span<const DrawItem> draws;
        };
    }  // namespace Render
}  // namespace Incubator
