#include "runtime/scene/camera.h"

#include <cmath>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>

#include "runtime/scene/detail/pose_validation.h"

namespace Incubator::Scene
{
    namespace
    {
        Perspective validPerspective(Perspective value)
        {
            if (!std::isfinite(value.verticalFov) || !std::isfinite(value.nearPlane) ||
                !std::isfinite(value.farPlane) || value.verticalFov <= 0 || value.verticalFov >= glm::pi<float>() ||
                value.nearPlane <= 0 || value.farPlane <= value.nearPlane)
                throw Error::Exception(Error::Code::CONFIG,
                                       "Camera requires finite 0 < vertical FOV < pi and 0 < near < far");
            return value;
        }
    }  // namespace

    Camera::Camera(glm::vec3 position, glm::quat rotation, Perspective perspective)
        : position_(Detail::finiteVector(position, "Camera position")),
          rotation_(Detail::unitRotation(rotation)),
          perspective_(validPerspective(perspective))
    {}

    const glm::vec3& Camera::position() const
    {
        return position_;
    }

    const glm::quat& Camera::rotation() const
    {
        return rotation_;
    }

    const Perspective& Camera::perspective() const
    {
        return perspective_;
    }

    void Camera::setPosition(glm::vec3 position)
    {
        position_ = Detail::finiteVector(position, "Camera position");
    }

    void Camera::setRotation(glm::quat rotation)
    {
        rotation_ = Detail::unitRotation(rotation);
    }

    void Camera::setPerspective(Perspective perspective)
    {
        perspective_ = validPerspective(perspective);
    }

    Render::CameraMatrices Camera::matrices(Render::FrameExtent extent) const
    {
        if (extent.width == 0 || extent.height == 0)
            throw Error::Exception(Error::Code::CONFIG, "Camera matrices require a nonzero render extent");
        const auto view = glm::translate(glm::mat4_cast(glm::conjugate(rotation_)), -position_);
        auto projection =
            glm::perspectiveRH_ZO(perspective_.verticalFov, static_cast<float>(extent.width) / extent.height,
                                  perspective_.nearPlane, perspective_.farPlane);
        projection[1][1] *= -1;
        return {view, projection};
    }
}  // namespace Incubator::Scene
