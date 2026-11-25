#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "runtime/core/path/path.h"

namespace Incubator::Resource::Detail
{
    enum class ImageFormat
    {
        Png,
        Jpeg
    };

    /// @brief 按扩展名识别受支持的 8 位图片格式。
    inline std::optional<ImageFormat> imageFormatFromPath(std::string_view path)
    {
        if (Path::hasExtension(path, "png"))
            return ImageFormat::Png;
        if (Path::hasExtension(path, "jpg") || Path::hasExtension(path, "jpeg"))
            return ImageFormat::Jpeg;
        return std::nullopt;
    }

    struct DecodedImage
    {
        uint32_t width;
        uint32_t height;
        std::vector<uint8_t> pixels;
    };

    /// @brief 校验指定图片格式及尺寸，并解码为紧密排列的 RGBA8。
    /// @throws Error::Exception 图像无效或超出大小限制时抛出 IO。
    DecodedImage decodeRgba8(std::span<const uint8_t> bytes, ImageFormat format);
}  // namespace Incubator::Resource::Detail
