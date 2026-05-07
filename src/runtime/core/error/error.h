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
        /// @brief 错误代码枚举
        ///
        /// 定义引擎中可能出现的错误分类，用于 Exception 构造和上层错误处理。
        enum class Code
        {
            UNKNOWN,  ///< 未知/未分类错误
            IO,       ///< 文件/输入输出错误
            CONFIG,   ///< 配置解析错误
            NETWORK   ///< 网络通信错误
        };

        /// @brief 基于 std::source_location 的异常类
        ///
        /// 继承自 std::exception，构造时自动捕获调用位置（文件名、行号、函数名）。
        /// what() 返回格式化后的完整错误信息。
        class Exception : public std::exception
        {
        public:
            /// @brief 构造异常
            /// @param code 错误代码
            /// @param message 用户定义的错误描述
            /// @param loc 调用位置（默认自动捕获当前调用点）
            Exception(Code code, std::string message, std::source_location loc = std::source_location::current());

            /// @brief 获取格式化错误信息
            /// @return 包含错误代码、消息和调用位置的完整字符串
            const char* what() const noexcept override;

            /// @brief 获取错误代码
            Code code() const noexcept
            {
                return code_;
            }

            /// @brief 获取错误描述
            const std::string& message() const noexcept
            {
                return message_;
            }

            /// @brief 获取调用位置
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

        /// @brief 错误代码转字符串
        /// @param code 错误代码
        /// @return 对应的字符串表示（如 Code::IO → "IO"）
        std::string toString(Code code);

        /// @brief 从完整文件路径中提取文件名
        /// @param path 完整路径（如 "/path/to/file.cpp"）
        /// @return 文件名部分（如 "file.cpp"）
        std::string_view getFileName(std::string_view path);
    }  // namespace Error

}  // namespace Incubator
