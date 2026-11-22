#pragma once

#include <cmath>

namespace Incubator::Render
{
    /// @brief 离屏视口色调映射模式。
    enum class ToneMappingMode
    {
        Off = 0,
        Reinhard = 1
    };

    /// @brief 离屏视口显示设置，由场景保存并传入渲染帧输入。
    struct DisplaySettings
    {
        static constexpr float MIN_EXPOSURE_EV = -8.0f;
        static constexpr float MAX_EXPOSURE_EV = 8.0f;

        ToneMappingMode toneMapping = ToneMappingMode::Reinhard;
        float exposureEv = 0.0f;

        bool valid() const noexcept
        {
            return (toneMapping == ToneMappingMode::Off || toneMapping == ToneMappingMode::Reinhard) &&
                   std::isfinite(exposureEv) && exposureEv >= MIN_EXPOSURE_EV && exposureEv <= MAX_EXPOSURE_EV;
        }

        bool operator==(const DisplaySettings&) const = default;
    };
}  // namespace Incubator::Render
