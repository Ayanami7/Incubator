#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>

namespace Incubator::Scene
{
    // Owned TRS value. Rotations are normalized on input; zero/negative scales are valid.
    // Invalid (nonfinite or zero-length rotation) input throws CONFIG without changing the value.
    class Transform
    {
    public:
        explicit Transform(glm::vec3 position = glm::vec3(0), glm::quat rotation = glm::quat(1, 0, 0, 0),
                           glm::vec3 scale = glm::vec3(1));

        const glm::vec3& position() const;
        const glm::quat& rotation() const;
        const glm::vec3& scale() const;
        void setPosition(glm::vec3 position);
        void setRotation(glm::quat rotation);
        void setScale(glm::vec3 scale);

        // Column vectors: T * R * S. Units are the consuming scene/model's units.
        glm::mat4 matrix() const;

    private:
        glm::vec3 position_;
        glm::quat rotation_;
        glm::vec3 scale_;
    };
}  // namespace Incubator::Scene
