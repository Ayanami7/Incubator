#pragma once

#include <glm/gtc/matrix_transform.hpp>

#include "runtime/render/frame_input.h"

namespace Incubator
{
    namespace Demo
    {
        struct SceneTransform
        {
            glm::vec3 position{0.0f};
            glm::vec3 rotationDegrees{0.0f};
            glm::vec3 scale{1.0f};
        };

        struct SceneCamera
        {
            glm::vec3 target{0.0f};
            float yawDegrees = 0.0f;
            float pitchDegrees = 15.0f;
            float distance = 5.0f;
            float fieldOfViewDegrees = 45.0f;
        };

        inline glm::mat4 makeSceneModel(const SceneTransform& transform)
        {
            auto model = glm::translate(glm::mat4(1.0f), transform.position);
            model = glm::rotate(model, glm::radians(transform.rotationDegrees.z), glm::vec3(0.0f, 0.0f, 1.0f));
            model = glm::rotate(model, glm::radians(transform.rotationDegrees.y), glm::vec3(0.0f, 1.0f, 0.0f));
            model = glm::rotate(model, glm::radians(transform.rotationDegrees.x), glm::vec3(1.0f, 0.0f, 0.0f));
            return glm::scale(model, transform.scale);
        }

        // Call only after Renderer supplies a drawable extent. The caller owns the draw list.
        inline Render::FrameInput makeSceneFrame(Render::FrameExtent extent, std::span<const Render::DrawItem> draws,
                                                 const SceneCamera& camera = {})
        {
            const float yaw = glm::radians(camera.yawDegrees);
            const float pitch = glm::radians(camera.pitchDegrees);
            const auto offset = camera.distance * glm::vec3(glm::cos(pitch) * glm::sin(yaw), glm::sin(pitch),
                                                            glm::cos(pitch) * glm::cos(yaw));
            const auto view = glm::lookAtRH(camera.target + offset, camera.target, glm::vec3(0.0f, 1.0f, 0.0f));
            auto projection = glm::perspectiveRH_ZO(glm::radians(camera.fieldOfViewDegrees),
                                                    static_cast<float>(extent.width) / extent.height, 0.1f, 50.0f);
            projection[1][1] *= -1.0f;
            return {extent, {view, projection}, draws};
        }
    }  // namespace Demo
}  // namespace Incubator
