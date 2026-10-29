#pragma once

#include "runtime/render/renderer.h"

namespace Incubator::App
{
#ifdef _DEBUG
    inline constexpr auto VALIDATION_MODE = Render::RendererValidationMode::Optional;
#else
    inline constexpr auto VALIDATION_MODE = Render::RendererValidationMode::Disabled;
#endif
}  // namespace Incubator::App
