#pragma once

#include <string>
#include <string_view>

namespace Incubator
{
    namespace Path
    {
        /// @brief 连接两个路径组件
        /// @param a 路径前缀
        /// @param b 路径后缀
        /// @return 用 '/' 连接后的路径（自动处理重复分隔符）
        std::string join(std::string_view a, std::string_view b);

        /// @brief 连接三个路径组件
        /// @param a 路径前缀
        /// @param b 路径中间段
        /// @param c 路径后缀
        /// @return 用 '/' 连接后的路径
        std::string join(std::string_view a, std::string_view b, std::string_view c);

        /// @brief 路径规范化：处理 . 和 ..，统一分隔符为 '/'
        /// @param path 待规范化的路径
        /// @return 规范化后的路径，Windows 盘符开头会保留
        std::string normalize(std::string_view path);

        // ---- 组件提取 ----

        /// @brief 提取文件名（含扩展名）
        /// @param path 完整路径
        /// @return 文件名部分，空路径返回空字符串
        std::string getFileName(std::string_view path);

        /// @brief 提取文件名（不含扩展名）
        /// @param path 完整路径
        /// @return 去除扩展名后的文件名
        std::string getStem(std::string_view path);

        /// @brief 提取文件扩展名
        /// @param path 完整路径
        /// @return 扩展名（不含点），无扩展名返回空字符串
        std::string getExtension(std::string_view path);

        /// @brief 提取父目录路径
        /// @param path 完整路径
        /// @return 父目录路径，无父目录返回空字符串
        std::string getParentPath(std::string_view path);

        // ---- 路径判断 ----

        /// @brief 判断路径是否为绝对路径
        /// @param path 待判断的路径
        /// @return 是否为绝对路径（检测 Windows 盘符或 UNC 前缀）
        bool isAbsolute(std::string_view path);

        /// @brief 判断文件扩展名是否匹配（大小写不敏感）
        /// @param path 完整路径
        /// @param ext 扩展名（可带或不带前导点）
        /// @return 扩展名是否匹配
        bool hasExtension(std::string_view path, std::string_view ext);
    }  // namespace Path
}  // namespace Incubator
