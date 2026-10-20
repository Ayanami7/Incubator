#pragma once

#include <glm/gtc/quaternion.hpp>

#include "runtime/render/frame_input.h"

namespace Incubator::Scene
{
    struct Perspective
    {
        // Radians, strictly between 0 and pi; finite near > 0 and far > near.
        float verticalFov = glm::radians(45.0f);
        float nearPlane = 0.1f;
        float farPlane = 100.0f;
    };

    // World rigid pose: local forward -Z, up +Y. No scale or orbit/controller state.
    // Rotation input follows Transform's normalization and CONFIG failure rules.
    class Camera
    {
    public:
        explicit Camera(glm::vec3 position = glm::vec3(0), glm::quat rotation = glm::quat(1, 0, 0, 0),
                        Perspective perspective = {});

        const glm::vec3& position() const;
        const glm::quat& rotation() const;
        const Perspective& perspective() const;
        void setPosition(glm::vec3 position);
        void setRotation(glm::quat rotation);
        void setPerspective(Perspective perspective);

        // Requires nonzero actual render extent (otherwise CONFIG); skip absent extents upstream.
        // Returns inverse world pose and RH/ZO perspective with final Y adaptation exactly once.
        Render::CameraMatrices matrices(Render::FrameExtent extent) const;

    private:
        glm::vec3 position_;
        glm::quat rotation_;
        Perspective perspective_;
    };
}  // namespace Incubator::Scene
