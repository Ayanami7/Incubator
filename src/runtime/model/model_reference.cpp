#include "runtime/model/model_reference.h"

#include "runtime/core/error/error.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/resource/resource_id.h"

namespace Incubator::Model
{
    ModelReference::ModelReference(std::string_view virtualPath, PrepareOptions options) : options_(options)
    {
        using namespace Render;
        const auto validFilter = [](TextureFilter value) {
            return value == TextureFilter::Nearest || value == TextureFilter::Linear;
        };
        if ((options.mipMode != TextureMipMode::BaseLevel && options.mipMode != TextureMipMode::FullChain) ||
            !validFilter(options.sampler.minFilter) || !validFilter(options.sampler.magFilter) ||
            (options.sampler.mipFilter != MipFilter::BaseLevel && options.sampler.mipFilter != MipFilter::Nearest &&
             options.sampler.mipFilter != MipFilter::Linear) ||
            (options.shading != ShadingMode::Unlit && options.shading != ShadingMode::Lambert))
            throw Error::Exception(Error::Code::RENDER, "Model preparation options are invalid");

        if (virtualPath.find('\0') != std::string_view::npos || !Utils::Utf8::isValid(virtualPath))
            throw Error::Exception(Error::Code::RENDER, "Model virtual path is not valid UTF-8");
        path_ = Resource::canonicalizeResourceKey(virtualPath);
        if (path_.empty())
            throw Error::Exception(Error::Code::RENDER, "Model virtual path is invalid");
    }
}  // namespace Incubator::Model
