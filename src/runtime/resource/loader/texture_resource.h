#pragma once

#include <cstdint>
#include <vector>

#include "runtime/resource/resource_object.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief 紧密排列、由 CPU 持有的 RGBA8 像素，按左上原点从上到下存储。
        /// RGB 字节采用 sRGB 编码；alpha 为线性直通值（未预乘）。
        /// 不转换颜色元数据。保留透明像素中的 RGB 值。
        class TextureResource final : public ResourceObject
        {
        public:
            static constexpr uint32_t MAX_DIMENSION = 4096;
            static constexpr uint64_t MAX_PIXEL_BYTES = 64u * 1024u * 1024u;

            TextureResource(ResourceId id, uint32_t width, uint32_t height, std::vector<uint8_t> pixels);

            uint32_t width() const noexcept
            {
                return width_;
            }

            uint32_t height() const noexcept
            {
                return height_;
            }

            const std::vector<uint8_t>& pixels() const noexcept
            {
                return pixels_;
            }

        private:
            uint32_t width_;
            uint32_t height_;
            std::vector<uint8_t> pixels_;
        };
    }  // namespace Resource
}  // namespace Incubator
