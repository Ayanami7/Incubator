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
            // Final right-handed, zero-to-one depth projection, including the application's Y adaptation.
            glm::mat4 projection;
        };

        struct DrawItem
        {
            Mesh mesh;
            Material material;
            glm::mat4 model;
        };

        // Required application input. The list is borrowed only during drawFrame and may be empty.
        struct FrameInput
        {
            FrameExtent extent;
            CameraMatrices camera;
            std::span<const DrawItem> draws;
        };
    }  // namespace Render
}  // namespace Incubator
