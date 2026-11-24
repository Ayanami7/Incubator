#pragma once

#include <cstdint>
#include <vector>

#include "runtime/resource/resource_object.h"

namespace Incubator::Resource
{
    /// @brief 浮点纹理维度类型。
    enum class FloatTextureDimension
    {
        Texture2D,  ///< 单层二维浮点纹理。
        CubeMap     ///< 六面立方体贴图（Vulkan 层序：+X, -X, +Y, -Y, +Z, -Z）。
    };

    /// @brief 线性 RGBA16F 浮点纹理的单级 mip 载荷。
    struct FloatTextureMip
    {
        uint32_t width = 0;
        uint32_t height = 0;
        /// @brief 按面序（0..faces-1）、每面从顶行到底行紧密排列的 IEEE-754 binary16 RGBA16F 载荷。
        std::vector<uint16_t> rgba16f;
    };

    /// @brief 由 CPU 持有的线性 RGBA16F 浮点纹理（支持 2D 与 Cubemap 多级 mip 链）。
    class FloatTextureResource final : public ResourceObject
    {
    public:
        static constexpr uint32_t MAX_DIMENSION = 4096;
        static constexpr uint64_t MAX_PIXEL_BYTES = 64u * 1024u * 1024u;

        FloatTextureResource(ResourceId id, FloatTextureDimension dimension, std::vector<FloatTextureMip> mips);

        FloatTextureDimension dimension() const noexcept
        {
            return dimension_;
        }

        bool isCubemap() const noexcept
        {
            return dimension_ == FloatTextureDimension::CubeMap;
        }

        uint32_t faceCount() const noexcept
        {
            return isCubemap() ? 6u : 1u;
        }

        uint32_t width() const noexcept
        {
            return mips_[0].width;
        }

        uint32_t height() const noexcept
        {
            return mips_[0].height;
        }

        uint32_t mipLevels() const noexcept
        {
            return static_cast<uint32_t>(mips_.size());
        }

        const std::vector<FloatTextureMip>& mips() const noexcept
        {
            return mips_;
        }

    private:
        FloatTextureDimension dimension_;
        std::vector<FloatTextureMip> mips_;
    };
}  // namespace Incubator::Resource
