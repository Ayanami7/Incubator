#pragma once

#include <optional>
#include <vector>

#include "runtime/render/frame_input.h"

namespace Incubator::Scene
{
    class Scene;

    // Owns one extracted frame; copies/moves never store a span into another frame's vector.
    struct SceneFrame
    {
        Render::FrameExtent extent{0, 0};
        Render::CameraMatrices camera{glm::mat4(1), glm::mat4(1)};
        std::vector<Render::DrawItem> draws;

        // Borrow only at submission. Keep this frame/storage alive until drawFrame returns.
        Render::FrameInput input() const&;
        Render::FrameInput input() const&& = delete;
    };

    // Zero width/height skips projection and extraction. No I/O, upload or GPU validation.
    std::optional<SceneFrame> extractSceneFrame(const Scene& scene, Render::FrameExtent extent);
}  // namespace Incubator::Scene
