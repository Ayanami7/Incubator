// error.h
#pragma once

#include <spdlog/fmt/fmt.h>

#include <exception>
#include <source_location>
#include <string>
#include <string_view>

namespace Incubator
{
    enum class ErrorCode
    {
        UNKNOWN,
        IO,
        CONFIG,
        NETWORK
    };

    class Exception : public std::exception
    {
    public:
        Exception(ErrorCode code, std::string message, std::source_location loc = std::source_location::current());

        const char* what() const noexcept override;
        ErrorCode code() const noexcept
        {
            return code_;
        }
        const std::string& message() const noexcept
        {
            return message_;
        }
        const std::source_location& location() const noexcept
        {
            return location_;
        }

    private:
        ErrorCode code_;
        std::string message_;
        std::source_location location_;
        mutable std::string what_cache_;  // 用于缓存 what() 字符串

        void buildWhat() const;
    };

    // 错误码转换为字符串
    std::string toString(ErrorCode code);

    // 从路径中提取文件名
    std::string_view getFileName(std::string_view path);

}  // namespace Incubator
