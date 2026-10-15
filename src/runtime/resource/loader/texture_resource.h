#pragma once

#include <cstdint>
#include <vector>

#include "runtime/resource/resource_object.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief Owned, tightly packed RGBA8 CPU pixels, top-to-bottom from the top-left origin.
        /// RGB bytes are sRGB encoded; alpha is linear, straight (not premultiplied).
        /// No color metadata conversion is applied. Transparent RGB is preserved.
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
