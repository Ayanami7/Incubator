#include "runtime/render/lighting.h"

#include <cmath>

namespace Incubator::Render
{
    namespace
    {
        constexpr float HALF_PI = 1.5707963267948966f;
    }

    bool LocalLight::valid() const noexcept
    {
        if (type != LocalLightType::Point && type != LocalLightType::Spot)
            return false;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!std::isfinite(position[axis]) || !std::isfinite(direction[axis]) || !std::isfinite(color[axis]) ||
                color[axis] < 0.0f || color[axis] > 1.0f)
                return false;
        }
        if (direction.x == 0.0f && direction.y == 0.0f && direction.z == 0.0f)
            return false;
        if (!std::isfinite(intensity) || intensity < 0.0f || !std::isfinite(range) || range <= 0.0f)
            return false;
        return std::isfinite(innerConeAngle) && std::isfinite(outerConeAngle) && innerConeAngle >= 0.0f &&
               innerConeAngle <= outerConeAngle && outerConeAngle < HALF_PI;
    }

    bool Lighting::valid() const noexcept
    {
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(direction[axis]) || !std::isfinite(color[axis]) || color[axis] < 0 || color[axis] > 1)
                return false;
        if ((direction.x == 0 && direction.y == 0 && direction.z == 0) || !std::isfinite(intensity) || intensity < 0 ||
            !std::isfinite(ambient) || ambient < 0 || ambient > 1)
            return false;
        if (localLights.size() > MAX_LOCAL_LIGHTS)
            return false;
        for (const auto& light : localLights)
        {
            if (!light.valid())
                return false;
        }
        return true;
    }
}  // namespace Incubator::Render
