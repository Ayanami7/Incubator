#include "runtime/resource/loader/texture_resource.h"

#include <utility>

#include "runtime/core/error/error.h"
#include "runtime/resource/resource_type.h"

namespace Incubator
{
    namespace Resource
    {
        TextureResource::TextureResource(ResourceId id, uint32_t width, uint32_t height, std::vector<uint8_t> pixels)
            : ResourceObject(id, ResourceType::TEXTURE), width_(width), height_(height), pixels_(std::move(pixels))
        {
            if (width == 0 || height == 0 || width > MAX_DIMENSION || height > MAX_DIMENSION)
            {
                throw Error::Exception(Error::Code::CONFIG, "Texture dimensions must be in [1, 4096]");
            }
            const uint64_t byteCount = static_cast<uint64_t>(width) * height * 4u;
            if (byteCount > MAX_PIXEL_BYTES || byteCount != pixels_.size())
            {
                throw Error::Exception(Error::Code::CONFIG, "Texture RGBA8 payload size does not match dimensions");
            }
        }
    }  // namespace Resource
}  // namespace Incubator
