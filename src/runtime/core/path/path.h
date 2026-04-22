#pragma once

#include <string>
#include <string_view>

namespace Incubator
{
    namespace Path
    {
        // 路径连接
        std::string join(std::string_view a, std::string_view b);
        std::string join(std::string_view a, std::string_view b, std::string_view c);

        // 路径规范化：处理 . 和 ..，统一分隔符
        std::string normalize(std::string_view path);

        // 组件提取
        std::string getFileName(std::string_view path);
        std::string getStem(std::string_view path);
        std::string getExtension(std::string_view path);
        std::string getParentPath(std::string_view path);

        // 路径判断
        bool isAbsolute(std::string_view path);
        bool hasExtension(std::string_view path, std::string_view ext);
    }
}
