#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Incubator
{
    namespace Utils
    {
        /// @brief UTF-8 编码边界工具
        ///
        /// 全引擎统一约定：std::string / std::string_view 均为 UTF-8 字节序列。
        /// 本模块是 std::filesystem::path 与 UTF-8 字符串互转的唯一入口，
        /// 避免直接调用 path::string()（MSVC 返回系统 ANSI 代码页编码，中文区域即 GBK）。
        namespace Utf8
        {
            /// @brief 校验字符串是否为合法 UTF-8
            /// @param s 待校验字符串
            /// @return 是否合法 UTF-8
            bool isValid(std::string_view s);

            /// @brief 将文件系统路径转换为 UTF-8 字节序列
            /// @param path 文件系统路径
            /// @return UTF-8 编码的路径字符串
            std::string toUtf8String(const std::filesystem::path& path);

            /// @brief 将 UTF-8 字符串构造为文件系统路径
            /// @param utf8 UTF-8 编码的路径字符串
            /// @return 文件系统路径
            /// @note 仅支持 UTF-8 输入；非法 UTF-8 行为未定义（调用方应先用 isValid 校验）
            std::filesystem::path toFsPath(std::string_view utf8);
        }
    }
}