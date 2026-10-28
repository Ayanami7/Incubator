#include "runtime/render/shadow.h"

#include <cmath>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include "runtime/core/error/error.h"

namespace Incubator::Render
{
    std::optional<ShadowFrame> makeShadowFrame(const ShadowSettings& settings, const glm::vec3& direction)
    {
        if (!settings.enabled)
            return std::nullopt;

        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(settings.center[axis]) || !std::isfinite(direction[axis]))
                throw Error::Exception(Error::Code::CONFIG, "Shadow center and light direction must be finite");
        for (int axis = 0; axis < 2; ++axis)
            if (!std::isfinite(settings.halfExtent[axis]) || settings.halfExtent[axis] <= 0)
                throw Error::Exception(Error::Code::CONFIG, "Shadow half extents must be finite and positive");
        if (!std::isfinite(settings.nearPlane) || !std::isfinite(settings.farPlane) || settings.nearPlane <= 0 ||
            settings.farPlane <= settings.nearPlane)
            throw Error::Exception(Error::Code::CONFIG, "Shadow depth range requires finite 0 < near < far");
        if (!std::isfinite(settings.depthBias) || settings.depthBias < 0)
            throw Error::Exception(Error::Code::CONFIG, "Shadow receiver depth bias must be finite and nonnegative");

        const double length = std::hypot(double(direction.x), double(direction.y), double(direction.z));
        if (length == 0)
            throw Error::Exception(Error::Code::CONFIG, "Shadow light direction must be nonzero");
        const auto forward = glm::dvec3(direction) / length;
        const glm::dvec3 up = std::abs(forward.y) > 0.99 ? glm::dvec3(0, 0, 1) : glm::dvec3(0, 1, 0);
        const auto eye =
            glm::dvec3(settings.center) - forward * ((double(settings.nearPlane) + double(settings.farPlane)) * 0.5);
        // 先构造朝向，再施加平移，避免大世界坐标下观察位置与目标点相减丢失方向。
        const auto view = glm::translate(glm::lookAtRH(glm::dvec3(0), forward, up), -eye);
        auto projection = glm::orthoRH_ZO(-double(settings.halfExtent.x), double(settings.halfExtent.x),
                                          -double(settings.halfExtent.y), double(settings.halfExtent.y),
                                          double(settings.nearPlane), double(settings.farPlane));
        projection[1][1] *= -1;
        const glm::mat4 matrix(projection * view);
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                if (!std::isfinite(matrix[column][row]))
                    throw Error::Exception(Error::Code::CONFIG,
                                           "Shadow view projection must be representable and finite");
        return ShadowFrame{matrix, settings.depthBias};
    }
}  // namespace Incubator::Render
