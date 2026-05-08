#include "runtime/platform/fileio/fileio.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "runtime/core/error/error.h"

#include <string>

namespace Incubator
{
    namespace FileIO
    {
        namespace Detail
        {

            // ============================================================
            //  Win32 API 转换辅助
            // ============================================================

            DWORD toWin32Access(OpenMode mode)
            {
                DWORD access = 0;
                if (hasFlag(mode, OpenMode::Read))
                    access |= GENERIC_READ;
                if (hasFlag(mode, OpenMode::Write))
                    access |= GENERIC_WRITE;
                return access;
            }

            DWORD toWin32CreationDisposition(OpenMode mode)
            {
                OpenMode flags = static_cast<OpenMode>(
                    static_cast<uint8_t>(mode) & ~(static_cast<uint8_t>(OpenMode::Read) | static_cast<uint8_t>(OpenMode::Write))
                );

                bool create = hasFlag(flags, OpenMode::Create);
                bool truncate = hasFlag(flags, OpenMode::Truncate);

                if (create && truncate)
                    return CREATE_ALWAYS;
                if (create)
                    return OPEN_ALWAYS;
                if (truncate)
                    return TRUNCATE_EXISTING;
                return OPEN_EXISTING;
            }

            DWORD toWin32ShareMode(ShareMode share)
            {
                DWORD flags = 0;
                if (share == ShareMode::Exclusive)
                    return 0;
                if (share == ShareMode::Read || share == ShareMode::ReadWrite)
                    flags |= FILE_SHARE_READ;
                if (share == ShareMode::Write || share == ShareMode::ReadWrite)
                    flags |= FILE_SHARE_WRITE;
                return flags;
            }

            DWORD toWin32MoveMethod(SeekOrigin origin)
            {
                switch (origin)
                {
                    case SeekOrigin::Begin:
                        return FILE_BEGIN;
                    case SeekOrigin::Current:
                        return FILE_CURRENT;
                    case SeekOrigin::End:
                        return FILE_END;
                    default:
                        return FILE_BEGIN;
                }
            }

            std::string formatWin32Error(DWORD errorCode, std::string_view context)
            {
                std::string msg(context);
                msg += " (error: ";
                msg += std::to_string(errorCode);
                msg += ")";
                return msg;
            }

            // ============================================================
            //  Win32FileHandle
            // ============================================================

            class Win32FileHandle : public FileHandle
            {
            public:
                explicit Win32FileHandle(HANDLE handle)
                    : handle_(handle)
                {
                }

                ~Win32FileHandle() override
                {
                    if (isValid())
                    {
                        close();
                    }
                }

                Win32FileHandle(const Win32FileHandle&) = delete;
                Win32FileHandle& operator=(const Win32FileHandle&) = delete;

                Win32FileHandle(Win32FileHandle&& other) noexcept
                    : handle_(other.handle_)
                {
                    other.handle_ = INVALID_HANDLE_VALUE;
                }

                Win32FileHandle& operator=(Win32FileHandle&& other) noexcept
                {
                    if (this != &other)
                    {
                        if (isValid())
                            close();
                        handle_ = other.handle_;
                        other.handle_ = INVALID_HANDLE_VALUE;
                    }
                    return *this;
                }

                bool isValid() const override
                {
                    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
                }

                size_t read(std::span<uint8_t> buffer) override
                {
                    if (!isValid())
                    {
                        throw Error::Exception(Error::Code::IO,
                                               "Read on invalid file handle");
                    }

                    DWORD bytesRead = 0;
                    DWORD toRead = static_cast<DWORD>(
                        std::min<size_t>(buffer.size(), static_cast<size_t>(UINT32_MAX))
                    );

                    if (!ReadFile(handle_, buffer.data(), toRead, &bytesRead, nullptr))
                    {
                        DWORD err = GetLastError();
                        throw Error::Exception(Error::Code::IO,
                                               formatWin32Error(err, "ReadFile failed"));
                    }

                    return static_cast<size_t>(bytesRead);
                }

                size_t write(std::span<const uint8_t> data) override
                {
                    if (!isValid())
                    {
                        throw Error::Exception(Error::Code::IO,
                                               "Write on invalid file handle");
                    }

                    DWORD bytesWritten = 0;
                    DWORD toWrite = static_cast<DWORD>(
                        std::min<size_t>(data.size(), static_cast<size_t>(UINT32_MAX))
                    );

                    if (!WriteFile(handle_, data.data(), toWrite, &bytesWritten, nullptr))
                    {
                        DWORD err = GetLastError();
                        throw Error::Exception(Error::Code::IO,
                                               formatWin32Error(err, "WriteFile failed"));
                    }

                    return static_cast<size_t>(bytesWritten);
                }

                size_t seek(int64_t offset, SeekOrigin origin) override
                {
                    if (!isValid())
                    {
                        throw Error::Exception(Error::Code::IO,
                                               "Seek on invalid file handle");
                    }

                    LARGE_INTEGER liOffset;
                    liOffset.QuadPart = offset;

                    LARGE_INTEGER liNewPtr;
                    if (!SetFilePointerEx(handle_, liOffset, &liNewPtr,
                                          toWin32MoveMethod(origin)))
                    {
                        DWORD err = GetLastError();
                        throw Error::Exception(Error::Code::IO,
                                               formatWin32Error(err, "SetFilePointerEx failed"));
                    }

                    return static_cast<size_t>(liNewPtr.QuadPart);
                }

                uint64_t tell() const override
                {
                    if (!isValid())
                    {
                        throw Error::Exception(Error::Code::IO,
                                               "Tell on invalid file handle");
                    }

                    LARGE_INTEGER liOffset;
                    liOffset.QuadPart = 0;

                    LARGE_INTEGER liNewPtr;
                    if (!SetFilePointerEx(handle_, liOffset, &liNewPtr, FILE_CURRENT))
                    {
                        DWORD err = GetLastError();
                        throw Error::Exception(Error::Code::IO,
                                               formatWin32Error(err, "SetFilePointerEx (tell) failed"));
                    }

                    return static_cast<uint64_t>(liNewPtr.QuadPart);
                }

                uint64_t size() const override
                {
                    if (!isValid())
                    {
                        throw Error::Exception(Error::Code::IO,
                                               "Size on invalid file handle");
                    }

                    LARGE_INTEGER fileSize;
                    if (!GetFileSizeEx(handle_, &fileSize))
                    {
                        DWORD err = GetLastError();
                        throw Error::Exception(Error::Code::IO,
                                               formatWin32Error(err, "GetFileSizeEx failed"));
                    }

                    return static_cast<uint64_t>(fileSize.QuadPart);
                }

                void close() override
                {
                    if (isValid())
                    {
                        CloseHandle(handle_);
                        handle_ = INVALID_HANDLE_VALUE;
                    }
                }

                void flush() override
                {
                    if (!isValid())
                    {
                        throw Error::Exception(Error::Code::IO,
                                               "Flush on invalid file handle");
                    }

                    if (!FlushFileBuffers(handle_))
                    {
                        DWORD err = GetLastError();
                        throw Error::Exception(Error::Code::IO,
                                               formatWin32Error(err, "FlushFileBuffers failed"));
                    }
                }

            private:
                HANDLE handle_ = INVALID_HANDLE_VALUE;
            };

            // ============================================================
            //  Win32FileIO
            // ============================================================

            class Win32FileIO : public FileIO
            {
            public:
                bool exists(const std::filesystem::path& path) override
                {
                    DWORD attrs = GetFileAttributesA(path.string().c_str());
                    return attrs != INVALID_FILE_ATTRIBUTES &&
                           !(attrs & FILE_ATTRIBUTE_DIRECTORY);
                }

                uint64_t getFileSize(const std::filesystem::path& path) override
                {
                    WIN32_FILE_ATTRIBUTE_DATA attr;
                    if (!GetFileAttributesExA(path.string().c_str(),
                                              GetFileExInfoStandard, &attr))
                    {
                        return 0;
                    }
                    return (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) |
                           static_cast<uint64_t>(attr.nFileSizeLow);
                }

                bool remove(const std::filesystem::path& path) override
                {
                    return DeleteFileA(path.string().c_str()) != FALSE;
                }

                std::unique_ptr<FileHandle> open(
                    const std::filesystem::path& path,
                    OpenMode mode,
                    ShareMode share) override
                {
                    auto pathStr = path.string();

                    HANDLE h = CreateFileA(
                        pathStr.c_str(),
                        toWin32Access(mode),
                        toWin32ShareMode(share),
                        nullptr,
                        toWin32CreationDisposition(mode),
                        FILE_ATTRIBUTE_NORMAL,
                        nullptr
                    );

                    if (h == INVALID_HANDLE_VALUE)
                    {
                        DWORD err = GetLastError();
                        // 文件不存在等预期情况直接返回空
                        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                        {
                            return nullptr;
                        }
                        throw Error::Exception(Error::Code::IO,
                                               formatWin32Error(err, "CreateFileA failed: " + pathStr));
                    }

                    return std::make_unique<Win32FileHandle>(h);
                }

                bool directoryExists(const std::filesystem::path& path) override
                {
                    DWORD attrs = GetFileAttributesA(path.string().c_str());
                    return attrs != INVALID_FILE_ATTRIBUTES &&
                           (attrs & FILE_ATTRIBUTE_DIRECTORY);
                }

                bool createDirectory(const std::filesystem::path& path) override
                {
                    return CreateDirectoryA(path.string().c_str(), nullptr) != FALSE;
                }

                bool removeDirectory(const std::filesystem::path& path) override
                {
                    return RemoveDirectoryA(path.string().c_str()) != FALSE;
                }
            };

        }  // namespace Detail

        std::unique_ptr<FileIO> createPlatformFileIO()
        {
            return std::make_unique<Detail::Win32FileIO>();
        }

    }  // namespace FileIO
}  // namespace Incubator
