#include "hash.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace Incubator
{
    namespace Utils
    {
        namespace Detail
        {
            Hash hashBytes(const void* data, size_t size) noexcept
            {
                Hash result = kOffset;
                const uint8_t* bytes = static_cast<const uint8_t*>(data);

                for (size_t i = 0; i < size; ++i)
                {
                    result ^= static_cast<Hash>(bytes[i]);
                    result *= kPrime;
                }

                return result;
            }
        }

        Hash hash(std::string_view str) noexcept
        {
            return Detail::hashBytes(str.data(), str.size());
        }

        Hash hashFileFast(const std::filesystem::path& path) noexcept
        {
            try
            {
                if (!std::filesystem::exists(path))
                {
                    return 0;
                }

                std::uintmax_t fileSize = std::filesystem::file_size(path);
                auto writeTime = std::filesystem::last_write_time(path);
                auto timeSinceEpoch = writeTime.time_since_epoch();
                auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(timeSinceEpoch).count();

                Hash seed = Detail::kOffset;
                combine(seed, path.string());
                combine(seed, fileSize);
                combine(seed, timestamp);

                return seed;
            }
            catch (...)
            {
                return 0;
            }
        }

        Hash hashFileContent(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                throw std::runtime_error("Failed to open file: " + path.string());
            }

            file.seekg(0, std::ios::end);
            std::streamsize fileSize = file.tellg();
            file.seekg(0, std::ios::beg);

            constexpr size_t bufferSize = 8192;
            std::vector<uint8_t> buffer(bufferSize);

            Hash result = Detail::kOffset;

            while (fileSize > 0)
            {
                std::streamsize toRead = std::min<std::streamsize>(fileSize, bufferSize);
                file.read(reinterpret_cast<char*>(buffer.data()), toRead);

                Hash chunkHash = Detail::hashBytes(buffer.data(), static_cast<size_t>(toRead));
                combine(result, chunkHash);

                fileSize -= toRead;
            }

            return result;
        }
    }
}
