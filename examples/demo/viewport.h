#pragma once
#include <algorithm>
#include <cmath>
#include <limits>

#include "runtime/render/frame_input.h"
namespace Incubator::Demo
{
    /// @brief 逻辑区域按两轴 framebuffer 比例换算，向下取整，正尺寸至少为一个像素。
    inline Render::FrameExtent viewportExtent(double width, double height, double scaleX, double scaleY)
    {
        const double w = width * scaleX, h = height * scaleY;
        if (!(w > 0 && h > 0) || !std::isfinite(w) || !std::isfinite(h) || w > std::numeric_limits<uint32_t>::max() ||
            h > std::numeric_limits<uint32_t>::max())
            return {};
        return {static_cast<uint32_t>(std::max(1.0, std::floor(w))),
                static_cast<uint32_t>(std::max(1.0, std::floor(h)))};
    }
}  // namespace Incubator::Demo
