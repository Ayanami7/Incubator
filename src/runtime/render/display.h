#pragma once

#include <cmath>

namespace Incubator::Render
{
    /// @brief 离屏视口色调映射模式。
    enum class ToneMappingMode
    {
        Off = 0,
        Reinhard = 1,
        AcesFilmic = 2
    };

    /// @brief 离屏视口显示设置，由场景保存并传入渲染帧输入。
    struct DisplaySettings
    {
        static constexpr float MIN_EXPOSURE_EV = -8.0f;
        static constexpr float MAX_EXPOSURE_EV = 8.0f;
        static constexpr float MAX_BLOOM_INTENSITY = 4.0f;

        ToneMappingMode toneMapping = ToneMappingMode::Reinhard;
        float exposureEv = 0.0f;
        bool skyboxEnabled = false;
        bool bloomEnabled = false;
        float bloomThreshold = 1.0f;
        float bloomIntensity = 0.35f;

        bool valid() const noexcept
        {
            return (toneMapping == ToneMappingMode::Off || toneMapping == ToneMappingMode::Reinhard ||
                    toneMapping == ToneMappingMode::AcesFilmic) &&
                   std::isfinite(exposureEv) && exposureEv >= MIN_EXPOSURE_EV && exposureEv <= MAX_EXPOSURE_EV &&
                   std::isfinite(bloomThreshold) && bloomThreshold >= 0.0f &&
                   std::isfinite(bloomIntensity) && bloomIntensity >= 0.0f && bloomIntensity <= MAX_BLOOM_INTENSITY;
        }

        bool operator==(const DisplaySettings&) const = default;
    };
}  // namespace Incubator::Render
