#pragma once

namespace Incubator::Render
{
    enum class TextureFilter
    {
        Nearest,
        Linear
    };
    enum class MipFilter
    {
        BaseLevel,
        Nearest,
        Linear
    };

    /// @brief 不可变的材质采样配置。
    /// @note U/V 方向的寻址方式保持为 Repeat。
    struct SamplerDesc
    {
        TextureFilter minFilter = TextureFilter::Nearest;
        TextureFilter magFilter = TextureFilter::Nearest;
        MipFilter mipFilter = MipFilter::BaseLevel;
        bool operator==(const SamplerDesc&) const = default;
    };
}  // namespace Incubator::Render
