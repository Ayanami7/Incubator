// error.h
#pragma once

#include <spdlog/fmt/fmt.h>

#include <exception>
#include <source_location>
#include <string>
#include <string_view>

namespace Incubator
{
    namespace Error
    {
        enum class Code
        {
            UNKNOWN,
            IO,
            CONFIG,
            NETWORK
        };

        class Exception : public std::exception
        {
        public:
            Exception(Code code, std::string message, std::source_location loc = std::source_location::current());

            const char* what() const noexcept override;
            Code code() const noexcept
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
            Code code_;
            std::string message_;
            std::source_location location_;
            mutable std::string what_cache_;

            void buildWhat() const;
        };

        std::string toString(Code code);

        std::string_view getFileName(std::string_view path);
    }  // namespace Error

}  // namespace Incubator
