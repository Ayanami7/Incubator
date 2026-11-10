#include "runtime/scene/camera.h"

#include <cmath>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_inverse.hpp>

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

    std::optional<Math::LineSegment> Camera::viewportSegment(Render::FrameExtent extent, glm::vec2 position) const
    {
        if (!extent.width || !extent.height || !std::isfinite(position.x) || !std::isfinite(position.y) ||
            position.x < 0 || position.x > 1 || position.y < 0 || position.y > 1)
            return std::nullopt;
        const auto camera = matrices(extent);
        const glm::dmat4 inverse = glm::inverse(glm::dmat4(camera.projection) * glm::dmat4(camera.view));
        const double x = 2.0 * position.x - 1.0, y = 2.0 * position.y - 1.0;
        auto point = [&](double z) -> std::optional<glm::vec3> {
            const auto homogeneous = inverse * glm::dvec4(x, y, z, 1.0);
            if (!std::isfinite(homogeneous.w) || homogeneous.w == 0.0)
                return std::nullopt;
            const glm::vec3 result(glm::dvec3(homogeneous) / homogeneous.w);
            if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z))
                return std::nullopt;
            return result;
        };
        const auto near = point(0.0), far = point(1.0);
        if (!near || !far)
            return std::nullopt;
        return Math::LineSegment{*near, *far};
    }
}  // namespace Incubator::Scene
