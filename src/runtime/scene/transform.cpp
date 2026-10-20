#include "runtime/scene/transform.h"

#include <glm/ext/matrix_transform.hpp>

#include "runtime/scene/detail/pose_validation.h"

namespace Incubator::Scene
{
    Transform::Transform(glm::vec3 position, glm::quat rotation, glm::vec3 scale)
        : position_(Detail::finiteVector(position, "Transform position")),
          rotation_(Detail::unitRotation(rotation)),
          scale_(Detail::finiteVector(scale, "Transform scale"))
    {}

    const glm::vec3& Transform::position() const
    {
        return position_;
    }

    const glm::quat& Transform::rotation() const
    {
        return rotation_;
    }

    const glm::vec3& Transform::scale() const
    {
        return scale_;
    }

    void Transform::setPosition(glm::vec3 position)
    {
        position_ = Detail::finiteVector(position, "Transform position");
    }

    void Transform::setRotation(glm::quat rotation)
    {
        rotation_ = Detail::unitRotation(rotation);
    }

    void Transform::setScale(glm::vec3 scale)
    {
        scale_ = Detail::finiteVector(scale, "Transform scale");
    }

    glm::mat4 Transform::matrix() const
    {
        return glm::scale(glm::translate(glm::mat4(1), position_) * glm::mat4_cast(rotation_), scale_);
    }
}  // namespace Incubator::Scene
