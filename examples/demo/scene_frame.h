#pragma once

#include <glm/gtc/matrix_transform.hpp>

#include "runtime/render/frame_input.h"

namespace Incubator
{
    namespace Demo
    {
        inline glm::mat4 makeSceneModel(float seconds)
        {
            auto model = glm::rotate(glm::mat4(1.0f), seconds * 0.8f, glm::vec3(0.0f, 1.0f, 0.0f));
            return glm::rotate(model, seconds * 0.55f, glm::vec3(1.0f, 0.0f, 0.0f));
        }

        // Call only after Renderer supplies a drawable extent. The caller owns the draw list.
        inline Render::FrameInput makeSceneFrame(Render::FrameExtent extent, std::span<const Render::DrawItem> draws)
        {
            const auto view = glm::lookAtRH(glm::vec3(2.5f, 1.9f, 3.2f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            auto projection = glm::perspectiveRH_ZO(glm::radians(45.0f),
                                                    static_cast<float>(extent.width) / extent.height, 0.1f, 10.0f);
            projection[1][1] *= -1.0f;
            return {extent, {view, projection}, draws};
        }
    }  // namespace Demo
}  // namespace Incubator
