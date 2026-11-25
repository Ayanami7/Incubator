#pragma once

#include <cstdint>
#include <glm/mat4x4.hpp>
#include <span>

#include "runtime/render/display.h"
#include "runtime/render/lighting.h"
#include "runtime/render/material.h"
#include "runtime/render/mesh.h"
#include "runtime/render/shadow.h"

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

        /// @brief 离屏 HDR 视口提交的单条 3D 调试辅助线段（世界空间端点与线性 RGB/透明度）。
        struct DebugLine
        {
            glm::vec3 start{0.0f};
            glm::vec3 end{0.0f};
            glm::vec3 color{1.0f};
            float alpha = 1.0f;
        };

        /// @brief 提交一帧渲染所需的应用输入。
        /// @note 两份绘制列表及调试线段列表仅在渲染提交调用期间借用，均可以为空。
        struct FrameInput
        {
            FrameExtent extent;
            CameraMatrices camera;
            std::span<const DrawItem> draws;
            Lighting lighting{};
            std::optional<ShadowFrame> shadow{};
            std::span<const DrawItem> shadowDraws{};  ///< 独立于主相机裁剪的投射物列表。
            DisplaySettings display{};
            bool iblEnabled = false;
            std::span<const DebugLine> debugLines{};  ///< 可选的 3D 调试线段列表（为空时零绘制开销）。
        };
    }  // namespace Render
}  // namespace Incubator
