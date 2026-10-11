#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Incubator
{
    namespace FileIO
    {
        class FileIO;
    }

    namespace Resource
    {
        namespace Loader
        {
            /// @brief Read a SPIR-V module into owned, host-endian 32-bit words.
            std::vector<uint32_t> readSpirv(FileIO::FileIO& fileIO, const std::filesystem::path& path);
        }  // namespace Loader
    }  // namespace Resource
}  // namespace Incubator
