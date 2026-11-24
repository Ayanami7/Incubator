#include "runtime/resource/loader/float_texture_resource.h"

#include <algorithm>
#include <bit>
#include <utility>

#include "runtime/core/error/error.h"
#include "runtime/resource/resource_type.h"

namespace Incubator::Resource
{
    FloatTextureResource::FloatTextureResource(ResourceId id, FloatTextureDimension dimension,
                                               std::vector<FloatTextureMip> mips)
        : ResourceObject(id, ResourceType::TEXTURE), dimension_(dimension), mips_(std::move(mips))
    {
        if (dimension_ != FloatTextureDimension::Texture2D && dimension_ != FloatTextureDimension::CubeMap)
            throw Error::Exception(Error::Code::CONFIG, "Float texture dimension is invalid");
        if (mips_.empty())
            throw Error::Exception(Error::Code::CONFIG, "Float texture requires at least one mip level");

        const uint32_t baseWidth = mips_[0].width;
        const uint32_t baseHeight = mips_[0].height;
        if (baseWidth == 0 || baseHeight == 0 || baseWidth > MAX_DIMENSION || baseHeight > MAX_DIMENSION)
            throw Error::Exception(Error::Code::CONFIG, "Float texture dimensions must be in [1, 4096]");
        if (dimension_ == FloatTextureDimension::CubeMap && baseWidth != baseHeight)
            throw Error::Exception(Error::Code::CONFIG, "Cubemap float texture requires square faces");

        const uint32_t maxMipLevels = std::bit_width(std::max(baseWidth, baseHeight));
        if (mips_.size() > maxMipLevels)
            throw Error::Exception(Error::Code::CONFIG, "Float texture mip level count exceeds dimensions");

        const uint64_t faces = faceCount();
        uint64_t expectedWidth = baseWidth;
        uint64_t expectedHeight = baseHeight;
        uint64_t totalBytes = 0;
        for (const auto& mip : mips_)
        {
            if (mip.width != expectedWidth || mip.height != expectedHeight)
                throw Error::Exception(Error::Code::CONFIG, "Float texture mip dimensions do not match mip chain");
            const uint64_t halfCount = faces * mip.width * mip.height * 4u;
            const uint64_t mipBytes = halfCount * sizeof(uint16_t);
            if (mip.rgba16f.size() != halfCount || mipBytes > MAX_PIXEL_BYTES - totalBytes)
                throw Error::Exception(Error::Code::CONFIG, "Float texture RGBA16F payload size does not match");
            totalBytes += mipBytes;
            expectedWidth = std::max<uint64_t>(1u, expectedWidth / 2u);
            expectedHeight = std::max<uint64_t>(1u, expectedHeight / 2u);
        }
    }
}  // namespace Incubator::Resource
