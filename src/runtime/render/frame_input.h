#pragma once

#include <cstdint>
#include <glm/mat4x4.hpp>

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

        // Required application input. Matrices are consumed/copied during drawFrame, never retained by reference.
        struct FrameInput
        {
            FrameExtent extent;
            CameraMatrices camera;
            glm::mat4 model;
        };
    }  // namespace Render
}  // namespace Incubator
