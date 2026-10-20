#pragma once

#include <cmath>
#include <glm/gtc/quaternion.hpp>
#include <string_view>

#include "runtime/core/error/error.h"

namespace Incubator::Scene::Detail
{
    inline glm::vec3 finiteVector(glm::vec3 value, std::string_view name)
    {
        if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
            throw Error::Exception(Error::Code::CONFIG, fmt::format("{} must be finite", name));
        return value;
    }

    inline glm::quat unitRotation(glm::quat value)
    {
        // Double precision keeps finite float inputs normalizable at both ends of their range.
        const auto wide = glm::dquat(value);
        const double length = glm::length(wide);
        if (!std::isfinite(length) || length == 0)
            throw Error::Exception(Error::Code::CONFIG, "Rotation must be a finite nonzero quaternion");
        return glm::quat(wide / length);
    }
}  // namespace Incubator::Scene::Detail
