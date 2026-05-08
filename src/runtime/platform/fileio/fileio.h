#pragma once

#include "runtime/platform/fileio/file_handle.h"

#include <filesystem>
#include <memory>

#include "runtime/platform/fileio/file_handle.h"

namespace Incubator
{
    namespace FileIO
    {
        /// @brief 文件打开模式（位标志组合）
        enum class OpenMode : uint8_t
        {
            Read = 1,
            Write = 2,
            ReadWrite = Read | Write,  // 3
            Create = 4,
            Truncate = 8
        };

        /// @brief 文件共享模式
        enum class ShareMode : uint8_t
        {
            Exclusive = 0,
            Read = 1,
            Write = 2,
            ReadWrite = Read | Write  // 3
        };

        /// @brief 文件 I/O 抽象基类
        ///
        /// 封装平台相关的文件系统操作，提供可 Mock 的接口。
        /// 使用 createPlatformFileIO() 工厂函数获取平台实例。
        class FileIO
        {
        public:
            virtual ~FileIO() = default;

            // ---- 文件操作 ----

            /// @brief 检查文件是否存在
            virtual bool exists(const std::filesystem::path& path) = 0;

            /// @brief 获取文件大小
            /// @return 字节数，文件不存在时返回 0
            virtual uint64_t getFileSize(const std::filesystem::path& path) = 0;

            /// @brief 删除文件
            /// @return 成功返回 true，文件不存在返回 false
            virtual bool remove(const std::filesystem::path& path) = 0;

            /// @brief 打开文件
            /// @param path 文件路径
            /// @param mode 打开模式（支持位组合：Read | Create 等）
            /// @param share 共享模式
            /// @return 文件句柄；文件不存在返回 nullptr，其他错误抛出 Error::Exception
            virtual std::unique_ptr<FileHandle> open(
                const std::filesystem::path& path,
                OpenMode mode,
                ShareMode share = ShareMode::Exclusive
            ) = 0;

            // ---- 目录操作 ----

            /// @brief 检查目录是否存在
            virtual bool directoryExists(const std::filesystem::path& path) = 0;

            /// @brief 创建目录（不会递归创建父目录）
            /// @return 成功返回 true，已存在返回 false
            virtual bool createDirectory(const std::filesystem::path& path) = 0;

            /// @brief 删除空目录
            /// @return 成功返回 true，目录不存在或不空返回 false
            virtual bool removeDirectory(const std::filesystem::path& path) = 0;

        protected:
            FileIO() = default;
        };

        // ---- 位运算重载 ----

        constexpr OpenMode operator|(OpenMode a, OpenMode b) noexcept
        {
            return static_cast<OpenMode>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
        }

        constexpr OpenMode operator&(OpenMode a, OpenMode b) noexcept
        {
            return static_cast<OpenMode>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
        }

        constexpr bool hasFlag(OpenMode value, OpenMode flag) noexcept
        {
            return (static_cast<uint8_t>(value) & static_cast<uint8_t>(flag)) != 0;
        }

        /// @brief 创建平台相关的 FileIO 实例
        /// @return unique_ptr<FileIO> 指向平台特定实现的实例
        std::unique_ptr<FileIO> createPlatformFileIO();

    }  // namespace FileIO
}  // namespace Incubator
