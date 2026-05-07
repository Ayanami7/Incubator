#pragma once

#include <cstdint>
#include <span>

namespace Incubator
{
    namespace FileIO
    {
        enum class SeekOrigin : uint8_t
        {
            Begin = 0,
            Current = 1,
            End = 2
        };

        /// @brief 文件句柄抽象基类
        ///
        /// 提供平台无关的文件读写操作接口。
        /// 每个派生类封装对应平台的文件句柄生命周期。
        class FileHandle
        {
        public:
            virtual ~FileHandle() = default;

            /// @brief 句柄是否有效
            virtual bool isValid() const = 0;

            /// @brief 从文件读取数据到缓冲区
            /// @param buffer 接收数据的缓冲区
            /// @return 实际读取的字节数（可能小于 buffer.size()）
            virtual size_t read(std::span<uint8_t> buffer) = 0;

            /// @brief 将数据写入文件
            /// @param data 待写入的数据
            /// @return 实际写入的字节数
            virtual size_t write(std::span<const uint8_t> data) = 0;

            /// @brief 移动文件读写指针
            /// @param offset 偏移量
            /// @param origin 参考位置
            /// @return 新的指针位置（相对于文件起始）
            virtual size_t seek(int64_t offset, SeekOrigin origin) = 0;

            /// @brief 获取当前读写指针位置
            /// @return 从文件起始到当前指针的字节数
            virtual uint64_t tell() const = 0;

            /// @brief 获取文件大小
            /// @return 文件总字节数
            virtual uint64_t size() const = 0;

            /// @brief 关闭文件句柄
            virtual void close() = 0;

            /// @brief 刷新缓冲区，确保数据写入物理存储
            virtual void flush() = 0;

            /// @brief 禁止拷贝
            FileHandle(const FileHandle&) = delete;
            FileHandle& operator=(const FileHandle&) = delete;

            /// @brief 允许移动
            FileHandle(FileHandle&&) noexcept = default;
            FileHandle& operator=(FileHandle&&) noexcept = default;

        protected:
            FileHandle() = default;
        };

    }  // namespace FileIO
}  // namespace Incubator
