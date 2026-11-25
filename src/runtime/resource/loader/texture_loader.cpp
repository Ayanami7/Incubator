#include "runtime/resource/loader/texture_loader.h"

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/path/path.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/resource/loader/detail/image_decode.h"
#include "runtime/resource/loader/float_texture_resource.h"
#include "runtime/resource/loader/texture_resource.h"

namespace Incubator
{
    namespace Resource
    {
        namespace
        {
            constexpr uint64_t MAX_INPUT_BYTES = 64u * 1024u * 1024u;
            static_assert(MAX_INPUT_BYTES <= std::numeric_limits<int>::max() &&
                          MAX_INPUT_BYTES <= std::numeric_limits<size_t>::max());
            constexpr std::array<uint8_t, 8> PNG_SIGNATURE = {137, 80, 78, 71, 13, 10, 26, 10};

            void checkInputSize(uint64_t size, std::string_view format)
            {
                if (size == 0 || size > MAX_INPUT_BYTES)
                {
                    throw Error::Exception(
                        Error::Code::IO, fmt::format("Invalid {} input size: {} (limit 64 MiB)", format, size));
                }
            }

            std::vector<uint8_t> readInput(FileIO::FileHandle& file, std::string_view format)
            {
                const uint64_t size = file.size();
                checkInputSize(size, format);
                std::vector<uint8_t> bytes(static_cast<size_t>(size));
                size_t offset = 0;
                while (offset < bytes.size())
                {
                    const size_t read = file.read(std::span<uint8_t>(bytes).subspan(offset));
                    if (read == 0)
                    {
                        throw Error::Exception(
                            Error::Code::IO,
                            fmt::format("Incomplete {} read: expected {}, actual {}", format, size, offset));
                    }
                    offset += read;
                }
                return bytes;
            }

            uint32_t readBigEndian(const uint8_t* bytes)
            {
                return (static_cast<uint32_t>(bytes[0]) << 24u) | (static_cast<uint32_t>(bytes[1]) << 16u) |
                       (static_cast<uint32_t>(bytes[2]) << 8u) | bytes[3];
            }

            struct ImageHeader
            {
                uint32_t width;
                uint32_t height;
                size_t pixelBytes;
            };

            ImageHeader checkPngHeader(std::span<const uint8_t> bytes)
            {
                if (bytes.size() < PNG_SIGNATURE.size() ||
                    !std::equal(PNG_SIGNATURE.begin(), PNG_SIGNATURE.end(), bytes.begin()))
                {
                    throw Error::Exception(Error::Code::IO, "Not a PNG file or incomplete PNG signature");
                }
                if (bytes.size() < 33)
                {
                    throw Error::Exception(Error::Code::IO, "Incomplete PNG IHDR");
                }
                constexpr std::array<uint8_t, 4> IHDR = {'I', 'H', 'D', 'R'};
                if (readBigEndian(bytes.data() + 8) != 13 || !std::equal(IHDR.begin(), IHDR.end(), bytes.begin() + 12))
                {
                    throw Error::Exception(Error::Code::IO, "Invalid PNG IHDR (must be the first chunk)");
                }
                const uint8_t colorType = bytes[25];
                if (bytes[24] != 8 || (colorType != 0 && colorType != 2 && colorType != 4 && colorType != 6) ||
                    bytes[26] != 0 || bytes[27] != 0 || bytes[28] != 0)
                {
                    throw Error::Exception(Error::Code::IO,
                                           fmt::format("Unsupported PNG input: depth {}, color {}, "
                                                       "compression {}, filter {}, interlace {}",
                                                       bytes[24], colorType, bytes[26], bytes[27], bytes[28]));
                }
                const uint32_t width = readBigEndian(bytes.data() + 16);
                const uint32_t height = readBigEndian(bytes.data() + 20);
                if (width == 0 || height == 0 || width > TextureResource::MAX_DIMENSION ||
                    height > TextureResource::MAX_DIMENSION)
                {
                    throw Error::Exception(Error::Code::IO,
                                           fmt::format("Invalid PNG dimensions: {}x{} (limit 4096)", width, height));
                }
                const uint64_t pixelBytes = static_cast<uint64_t>(width) * height * 4u;
                if (pixelBytes > TextureResource::MAX_PIXEL_BYTES || pixelBytes > std::numeric_limits<size_t>::max())
                {
                    throw Error::Exception(Error::Code::IO, "PNG RGBA8 output size exceeds supported limits");
                }
                return {width, height, static_cast<size_t>(pixelBytes)};
            }

            ImageHeader checkJpegHeader(std::span<const uint8_t> bytes)
            {
                if (bytes.size() < 2 || bytes[0] != 0xff || bytes[1] != 0xd8)
                    throw Error::Exception(Error::Code::IO, "Not a JPEG file or incomplete JPEG SOI");
                // stb_image 可容忍扫描数据提前结束，因此先确认文件以 JPEG 结束标记收尾。
                if (bytes.size() < 4 || bytes[bytes.size() - 2] != 0xff || bytes.back() != 0xd9)
                    throw Error::Exception(Error::Code::IO, "Incomplete JPEG: missing final EOI marker");
                int width = 0;
                int height = 0;
                if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, nullptr))
                {
                    const auto* reason = stbi_failure_reason();
                    throw Error::Exception(Error::Code::IO,
                                           fmt::format("Invalid JPEG header ({})", reason ? reason : "unknown error"));
                }
                if (width <= 0 || height <= 0 || width > TextureResource::MAX_DIMENSION ||
                    height > TextureResource::MAX_DIMENSION)
                {
                    throw Error::Exception(
                        Error::Code::IO, fmt::format("Invalid JPEG dimensions: {}x{} (limit 4096)", width, height));
                }
                const auto pixelBytes = static_cast<uint64_t>(width) * height * 4u;
                if (pixelBytes > TextureResource::MAX_PIXEL_BYTES || pixelBytes > std::numeric_limits<size_t>::max())
                    throw Error::Exception(Error::Code::IO, "JPEG RGBA8 output size exceeds supported limits");
                return {static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<size_t>(pixelBytes)};
            }

            std::string_view formatName(Detail::ImageFormat format)
            {
                return format == Detail::ImageFormat::Png ? "PNG" : "JPEG";
            }

            constexpr std::array<uint8_t, 12> KTX1_MAGIC = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31,
                                                            0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

            uint32_t readLittleEndian32(const uint8_t* bytes)
            {
                return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8u) |
                       (static_cast<uint32_t>(bytes[2]) << 16u) | (static_cast<uint32_t>(bytes[3]) << 24u);
            }

            std::unique_ptr<FloatTextureResource> decodeKtx1(const ResourceId& id, std::string_view pathUtf8,
                                                             std::span<const uint8_t> bytes)
            {
                if (bytes.size() < 64 || !std::equal(KTX1_MAGIC.begin(), KTX1_MAGIC.end(), bytes.begin()))
                    throw Error::Exception(Error::Code::IO, "Not a KTX1 file or incomplete KTX1 header");

                const uint32_t endianness = readLittleEndian32(bytes.data() + 12);
                const uint32_t glType = readLittleEndian32(bytes.data() + 16);
                const uint32_t glTypeSize = readLittleEndian32(bytes.data() + 20);
                const uint32_t glFormat = readLittleEndian32(bytes.data() + 24);
                const uint32_t glInternalFormat = readLittleEndian32(bytes.data() + 28);
                const uint32_t glBaseInternalFormat = readLittleEndian32(bytes.data() + 32);
                const uint32_t width = readLittleEndian32(bytes.data() + 36);
                const uint32_t height = readLittleEndian32(bytes.data() + 40);
                const uint32_t depth = readLittleEndian32(bytes.data() + 44);
                const uint32_t arrayElements = readLittleEndian32(bytes.data() + 48);
                const uint32_t faces = readLittleEndian32(bytes.data() + 52);
                const uint32_t mipLevels = readLittleEndian32(bytes.data() + 56);
                const uint32_t keyValueBytes = readLittleEndian32(bytes.data() + 60);

                if (endianness != 0x04030201u || glType != 0x140Bu || glTypeSize != 2u || glFormat != 0x1908u ||
                    glInternalFormat != 0x881Au || glBaseInternalFormat != 0x1908u)
                {
                    throw Error::Exception(Error::Code::IO,
                                           "Unsupported KTX1 format (requires little-endian GL_RGBA16F)");
                }
                if (depth != 0 || arrayElements != 0 || (faces != 1u && faces != 6u))
                    throw Error::Exception(Error::Code::IO, "Unsupported KTX1 texture dimensionality or face count");
                if (width == 0 || height == 0 || width > FloatTextureResource::MAX_DIMENSION ||
                    height > FloatTextureResource::MAX_DIMENSION)
                {
                    throw Error::Exception(Error::Code::IO, "Invalid KTX1 dimensions (limit 4096)");
                }
                if (faces == 6u && width != height)
                    throw Error::Exception(Error::Code::IO, "KTX1 cubemap requires square faces");
                const uint32_t maxMipLevels = std::bit_width(std::max(width, height));
                if (mipLevels == 0 || mipLevels > maxMipLevels)
                    throw Error::Exception(Error::Code::IO, "Invalid KTX1 mipmap level count");

                size_t offset = 64;
                if (keyValueBytes > bytes.size() - offset)
                    throw Error::Exception(Error::Code::IO, "Truncated KTX1 key-value metadata");
                offset += keyValueBytes;

                std::vector<FloatTextureMip> mips;
                mips.reserve(mipLevels);
                uint32_t mipWidth = width;
                uint32_t mipHeight = height;
                uint64_t totalBytes = 0;
                for (uint32_t level = 0; level < mipLevels; ++level)
                {
                    if (bytes.size() - offset < 4)
                        throw Error::Exception(Error::Code::IO, "Truncated KTX1 mip level size");
                    const uint32_t imageSize = readLittleEndian32(bytes.data() + offset);
                    offset += 4;
                    const uint64_t faceBytes = static_cast<uint64_t>(mipWidth) * mipHeight * 8u;
                    if (imageSize != faceBytes)
                        throw Error::Exception(Error::Code::IO, "KTX1 mip imageSize does not match RGBA16F extent");
                    if (faceBytes * faces > FloatTextureResource::MAX_PIXEL_BYTES - totalBytes)
                        throw Error::Exception(Error::Code::IO, "KTX1 RGBA16F payload size exceeds supported limits");
                    totalBytes += faceBytes * faces;

                    const uint32_t cubePadding = (4u - (imageSize % 4u)) % 4u;
                    FloatTextureMip mip{mipWidth, mipHeight, {}};
                    mip.rgba16f.resize(static_cast<size_t>(faces) * mipWidth * mipHeight * 4u);
                    const size_t faceHalfCount = static_cast<size_t>(mipWidth) * mipHeight * 4u;
                    for (uint32_t face = 0; face < faces; ++face)
                    {
                        if (bytes.size() - offset < faceBytes)
                            throw Error::Exception(Error::Code::IO, "Truncated KTX1 mip face payload");
                        std::memcpy(mip.rgba16f.data() + static_cast<size_t>(face) * faceHalfCount,
                                    bytes.data() + offset, static_cast<size_t>(faceBytes));
                        offset += static_cast<size_t>(faceBytes);
                        if (faces == 6u && cubePadding > 0)
                        {
                            if (bytes.size() - offset < cubePadding)
                                throw Error::Exception(Error::Code::IO, "Truncated KTX1 cube padding");
                            offset += cubePadding;
                        }
                    }
                    if (faces == 1u)
                    {
                        const uint32_t mipPadding = (4u - (imageSize % 4u)) % 4u;
                        if (bytes.size() - offset < mipPadding)
                            throw Error::Exception(Error::Code::IO, "Truncated KTX1 mip padding");
                        offset += mipPadding;
                    }
                    mips.push_back(std::move(mip));
                    mipWidth = std::max(1u, mipWidth / 2u);
                    mipHeight = std::max(1u, mipHeight / 2u);
                }
                if (offset != bytes.size())
                    throw Error::Exception(Error::Code::IO, "Unexpected trailing bytes in KTX1 file");

                Log::Registry::get("resource")
                    .debug("Decoded KTX1 '{}': {}x{}, faces={}, mips={}, {} input bytes, {} RGBA16F bytes", pathUtf8,
                           width, height, faces, mipLevels, bytes.size(), totalBytes);
                return std::make_unique<FloatTextureResource>(
                    id, faces == 6u ? FloatTextureDimension::CubeMap : FloatTextureDimension::Texture2D,
                    std::move(mips));
            }
        }  // namespace

        Detail::DecodedImage Detail::decodeRgba8(std::span<const uint8_t> bytes, ImageFormat format)
        {
            const auto name = formatName(format);
            checkInputSize(bytes.size(), name);
            const auto header = format == ImageFormat::Png ? checkPngHeader(bytes) : checkJpegHeader(bytes);
            int width = 0;
            int height = 0;
            std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(
                stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, nullptr, 4),
                &stbi_image_free);
            if (!decoded)
            {
                const char* reason = stbi_failure_reason();
                throw Error::Exception(Error::Code::IO,
                                       fmt::format("{} decode failed ({})",
                                                   name, reason && *reason ? reason : "decoder supplied no reason"));
            }
            if (width != static_cast<int>(header.width) || height != static_cast<int>(header.height))
            {
                throw Error::Exception(Error::Code::IO,
                                       format == ImageFormat::Png ? "PNG decoded dimensions do not match IHDR"
                                                                  : "JPEG decoded dimensions do not match header");
            }
            return {header.width, header.height, std::vector<uint8_t>(decoded.get(), decoded.get() + header.pixelBytes)};
        }

        TextureLoader::TextureLoader(AssetSource& source) : source_(source) {}

        std::unique_ptr<ResourceObject> TextureLoader::load(const ResourceId& id, std::string_view virtualPath)
        {
            const std::string pathUtf8 = Utils::Utf8::toUtf8String(source_.resolve(virtualPath));
            try
            {
                const bool isKtx = Path::hasExtension(virtualPath, "ktx");
                const auto format = Detail::imageFormatFromPath(virtualPath).value_or(Detail::ImageFormat::Png);
                const auto name = isKtx ? std::string_view{"KTX"} : formatName(format);
                auto file = source_.open(virtualPath);
                if (!file)
                {
                    throw Error::Exception(Error::Code::IO, fmt::format("{} file was not found", name));
                }
                const auto bytes = readInput(*file, name);
                if (isKtx)
                    return decodeKtx1(id, pathUtf8, bytes);
                auto image = Detail::decodeRgba8(bytes, format);
                Log::Registry::get("resource")
                    .debug("Decoded {} '{}': {}x{}, {} input bytes, {} RGBA8 bytes", name, pathUtf8, image.width,
                           image.height, bytes.size(), image.pixels.size());
                return std::make_unique<TextureResource>(id, image.width, image.height, std::move(image.pixels));
            }
            catch (const Error::Exception& ex)
            {
                throw Error::Exception(Error::Code::IO, fmt::format("TextureLoader '{}': {}", pathUtf8, ex.message()));
            }
            catch (const std::exception& ex)
            {
                throw Error::Exception(Error::Code::IO, fmt::format("TextureLoader '{}': {}", pathUtf8, ex.what()));
            }
        }
    }  // namespace Resource
}  // namespace Incubator
