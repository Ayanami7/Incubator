#include "runtime/resource/loader/spirv_reader.h"

#include <algorithm>
#include <limits>
#include <span>
#include <string>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/platform/fileio/fileio.h"

namespace Incubator
{
    namespace Resource
    {
        namespace Loader
        {
            namespace
            {
                constexpr uint32_t SPIRV_MAGIC = 0x07230203u;
                constexpr uint32_t SWAPPED_SPIRV_MAGIC = 0x03022307u;
                constexpr uint64_t SPIRV_HEADER_SIZE = 5u * sizeof(uint32_t);

                uint32_t byteSwap(uint32_t value) noexcept
                {
                    return ((value & 0x000000ffu) << 24u) | ((value & 0x0000ff00u) << 8u) |
                           ((value & 0x00ff0000u) >> 8u) | ((value & 0xff000000u) >> 24u);
                }
            }  // namespace

            std::vector<uint32_t> readSpirv(FileIO::FileIO& fileIO, const std::filesystem::path& path)
            {
                const std::string pathUtf8 = Utils::Utf8::toUtf8String(path);
                auto file = fileIO.open(path, FileIO::OpenMode::Read, FileIO::ShareMode::Read);
                if (!file)
                {
                    throw Error::Exception(Error::Code::IO,
                                           fmt::format("SPIR-V file was not found: {}", pathUtf8));
                }

                const uint64_t byteCount = file->size();
                if (byteCount < SPIRV_HEADER_SIZE || byteCount % sizeof(uint32_t) != 0 ||
                    byteCount > std::numeric_limits<size_t>::max())
                {
                    throw Error::Exception(Error::Code::IO,
                                           fmt::format("Invalid SPIR-V file size: {}", pathUtf8));
                }

                std::vector<uint32_t> words(static_cast<size_t>(byteCount / sizeof(uint32_t)));
                auto bytes = std::span<uint8_t>(reinterpret_cast<uint8_t*>(words.data()),
                                                static_cast<size_t>(byteCount));
                size_t offset = 0;
                while (offset < bytes.size())
                {
                    const size_t bytesRead = file->read(bytes.subspan(offset));
                    if (bytesRead == 0)
                    {
                        throw Error::Exception(Error::Code::IO,
                                               fmt::format("Incomplete SPIR-V read: {}", pathUtf8));
                    }
                    offset += bytesRead;
                }

                const bool byteSwapped = words.front() == SWAPPED_SPIRV_MAGIC;
                if (byteSwapped)
                {
                    std::transform(words.begin(), words.end(), words.begin(), byteSwap);
                }
                if (words.front() != SPIRV_MAGIC)
                {
                    throw Error::Exception(Error::Code::IO,
                                           fmt::format("Invalid SPIR-V magic number: {}", pathUtf8));
                }
                Log::Registry::get("resource").debug("Read SPIR-V '{}': {} bytes, byte order {}", pathUtf8,
                                                      byteCount, byteSwapped ? "swapped to native" : "native");
                return words;
            }
        }  // namespace Loader
    }  // namespace Resource
}  // namespace Incubator
