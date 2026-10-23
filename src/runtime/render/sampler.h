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

    // Immutable material sampling policy. U/V addressing remains Repeat.
    struct SamplerDesc
    {
        TextureFilter minFilter = TextureFilter::Nearest;
        TextureFilter magFilter = TextureFilter::Nearest;
        MipFilter mipFilter = MipFilter::BaseLevel;
        bool operator==(const SamplerDesc&) const = default;
    };
}  // namespace Incubator::Render
