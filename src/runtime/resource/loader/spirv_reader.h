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
            /// @brief 将 SPIR-V 模块读取为由 CPU 持有的本机字节序 32 位字数据。
            std::vector<uint32_t> readSpirv(FileIO::FileIO& fileIO, const std::filesystem::path& path);
        }  // namespace Loader
    }  // namespace Resource
}  // namespace Incubator
