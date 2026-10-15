#include "runtime/resource/loader/texture_loader.h"

#include <stb_image.h>

#include <algorithm>
#include <array>
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

            std::vector<uint8_t> readInput(FileIO::FileHandle& file)
            {
                const uint64_t size = file.size();
                if (size == 0 || size > MAX_INPUT_BYTES)
                {
                    throw Error::Exception(Error::Code::IO,
                                           fmt::format("Invalid PNG input size: {} (limit 64 MiB)", size));
                }
                std::vector<uint8_t> bytes(static_cast<size_t>(size));
                size_t offset = 0;
                while (offset < bytes.size())
                {
                    const size_t read = file.read(std::span<uint8_t>(bytes).subspan(offset));
                    if (read == 0)
                    {
                        throw Error::Exception(Error::Code::IO,
                                               fmt::format("Incomplete PNG read: expected {}, actual {}",
                                                           size, offset));
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

            struct PngHeader
            {
                uint32_t width;
                uint32_t height;
                size_t pixelBytes;
            };

            PngHeader checkHeader(std::span<const uint8_t> bytes)
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
                if (readBigEndian(bytes.data() + 8) != 13 ||
                    !std::equal(IHDR.begin(), IHDR.end(), bytes.begin() + 12))
                {
                    throw Error::Exception(Error::Code::IO, "Invalid PNG IHDR (must be the first chunk)");
                }
                const uint8_t colorType = bytes[25];
                if (bytes[24] != 8 || (colorType != 0 && colorType != 2 && colorType != 4 && colorType != 6) ||
                    bytes[26] != 0 || bytes[27] != 0 || bytes[28] != 0)
                {
                    throw Error::Exception(
                        Error::Code::IO,
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
        }  // namespace

        TextureLoader::TextureLoader(FileIO::FileIO& fileIO) : fileIO_(fileIO) {}

        std::unique_ptr<ResourceObject> TextureLoader::load(const ResourceId& id, std::string_view virtualPath)
        {
            const std::string pathUtf8 = Path::join("assets", virtualPath);
            try
            {
                const auto path = Utils::Utf8::toFsPath(pathUtf8);
                auto file = fileIO_.open(path, FileIO::OpenMode::Read, FileIO::ShareMode::Read);
                if (!file)
                {
                    throw Error::Exception(Error::Code::IO, "PNG file was not found");
                }
                const auto bytes = readInput(*file);
                const auto header = checkHeader(bytes);
                int width = 0;
                int height = 0;
                std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(
                    stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, nullptr, 4),
                    &stbi_image_free);
                if (!decoded)
                {
                    const char* reason = stbi_failure_reason();
                    throw Error::Exception(Error::Code::IO,
                                           fmt::format("PNG decode failed ({})",
                                                       reason && *reason ? reason : "decoder supplied no reason"));
                }
                if (width != static_cast<int>(header.width) || height != static_cast<int>(header.height))
                {
                    throw Error::Exception(Error::Code::IO, "PNG decoded dimensions do not match IHDR");
                }
                std::vector<uint8_t> pixels(decoded.get(), decoded.get() + header.pixelBytes);
                Log::Registry::get("resource").debug("Decoded PNG '{}': {}x{}, {} input bytes, {} RGBA8 bytes",
                                                      pathUtf8, header.width, header.height, bytes.size(),
                                                      header.pixelBytes);
                return std::make_unique<TextureResource>(id, header.width, header.height, std::move(pixels));
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
