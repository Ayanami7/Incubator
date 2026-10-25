#include "runtime/render/lighting.h"

#include <cmath>

namespace Incubator::Render
{
    bool Lighting::valid() const noexcept
    {
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(direction[axis]) || !std::isfinite(color[axis]) || color[axis] < 0 || color[axis] > 1)
                return false;
        return (direction.x != 0 || direction.y != 0 || direction.z != 0) && std::isfinite(intensity) &&
               intensity >= 0 && std::isfinite(ambient) && ambient >= 0 && ambient <= 1;
    }
}  // namespace Incubator::Render
