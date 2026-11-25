#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

namespace Incubator::FileIO
{
    class FileIO;
}

namespace Incubator::Editor
{
    /// @brief 将紧密排列的 RGBA8 像素写出为标准无压缩 DEFLATE 块 PNG 文件（零外部编码依赖）。
    /// @throws Error::Exception 尺寸无效、像素缓冲大小不匹配或文件写入失败时抛出 IO/CONFIG。
    void writeRgba8Png(FileIO::FileIO& fileIO, const std::filesystem::path& path, uint32_t width, uint32_t height,
                       std::span<const uint8_t> rgba8);
}  // namespace Incubator::Editor
