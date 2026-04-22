#include "runtime/core/path/path.h"

#include <cctype>
#include <vector>

namespace Incubator
{
    namespace Path
    {
        namespace Detail
        {
            constexpr char kSeparator = '/';

            // 检查是否为绝对路径
            inline bool isAbsoluteImpl(std::string_view path)
            {
                if (path.empty())
                {
                    return false;
                }

                // Windows: 检查盘符开头 (C:) 或 UNC 路径 (//)
                if (path.size() >= 2 && path[1] == ':' &&
                    ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')))
                {
                    return true;
                }
                if (path.size() >= 2 && path[0] == kSeparator && path[1] == kSeparator)
                {
                    return true;
                }

                return false;
            }

            // 分割路径为组件
            inline std::vector<std::string_view> splitPath(std::string_view path)
            {
                std::vector<std::string_view> components;
                size_t start = 0;
                size_t pos = path.find(kSeparator);

                while (pos != std::string_view::npos)
                {
                    if (pos > start)
                    {
                        components.push_back(path.substr(start, pos - start));
                    }
                    start = pos + 1;
                    pos = path.find(kSeparator, start);
                }

                if (start < path.size())
                {
                    components.push_back(path.substr(start));
                }

                return components;
            }

            // 连接路径组件
            inline std::string joinComponents(const std::vector<std::string_view>& components)
            {
                if (components.empty())
                {
                    return "";
                }

                std::string result;
                for (size_t i = 0; i < components.size(); ++i)
                {
                    if (i > 0)
                    {
                        result += kSeparator;
                    }
                    result += components[i];
                }
                return result;
            }

            // 大小写不敏感的字符串比较
            inline bool equalsIgnoreCase(std::string_view a, std::string_view b)
            {
                if (a.size() != b.size())
                {
                    return false;
                }

                for (size_t i = 0; i < a.size(); ++i)
                {
                    if (std::tolower(static_cast<unsigned char>(a[i])) !=
                        std::tolower(static_cast<unsigned char>(b[i])))
                    {
                        return false;
                    }
                }
                return true;
            }
        }

        // 路径连接
        std::string join(std::string_view a, std::string_view b)
        {
            std::string result(a);

            // 检查是否需要添加分隔符
            if (!result.empty() && result.back() != Detail::kSeparator && !b.empty() &&
                b.front() != Detail::kSeparator)
            {
                result += Detail::kSeparator;
            }
            else if (!result.empty() && result.back() == Detail::kSeparator && !b.empty() &&
                     b.front() == Detail::kSeparator)
            {
                // a 以分隔符结尾，b 以分隔符开头，去除 b 的前导分隔符
                b = b.substr(1);
            }

            result += b;
            return result;
        }

        std::string join(std::string_view a, std::string_view b, std::string_view c)
        {
            return join(join(a, b), c);
        }

        // 路径规范化
        std::string normalize(std::string_view path)
        {
            if (path.empty())
            {
                return "";
            }

            // 检查是否为绝对路径
            bool absolute = Detail::isAbsoluteImpl(path);

            // 将 \ 替换为 /
            std::string normalizedPath;
            normalizedPath.reserve(path.size());
            for (char c : path)
            {
                if (c == '\\')
                {
                    normalizedPath += Detail::kSeparator;
                }
                else
                {
                normalizedPath += c;
                }
            }

            // 分割路径
            auto components = Detail::splitPath(normalizedPath);

            // 处理 . 和 ..
            std::vector<std::string_view> result;
            for (const auto& component : components)
            {
                if (component == ".")
                {
                    // 跳过当前目录
                }
                else if (component == "..")
                {
                    // 处理上级目录
                    if (!result.empty())
                    {
                        bool canRemove = true;
                        // Windows: 不能越过盘符
                        if (result.size() == 1 && result.back().size() == 2 &&
                            result.back()[1] == ':')
                        {
                            canRemove = false;
                        }
                        if (canRemove)
                        {
                            result.pop_back();
                        }
                        else
                        {
                            result.push_back(component);
                        }
                    }
                    else if (!absolute)
                    {
                        result.push_back(component);
                    }
                }
                else
                {
                    result.push_back(component);
                }
            }

            // 重建路径
            std::string finalPath;
            finalPath.reserve(path.size());

            // Windows: 如果是绝对路径，检查盘符
            if (absolute && !result.empty() && result[0].size() >= 2 &&
                result[0][1] == ':')
            {
                finalPath += result[0];
                finalPath += Detail::kSeparator;
                result.erase(result.begin());
            }
            else if (absolute && path.size() >= 2 && path[0] == Detail::kSeparator &&
                     path[1] == Detail::kSeparator)
            {
                finalPath += "//";
                if (!result.empty())
                {
                    finalPath += result[0];
                    finalPath += Detail::kSeparator;
                    result.erase(result.begin());
                }
            }
            else if (absolute)
            {
                finalPath += Detail::kSeparator;
            }

            finalPath += Detail::joinComponents(result);
            return finalPath;
        }

        // 获取文件名
        std::string getFileName(std::string_view path)
        {
            if (path.empty())
            {
                return "";
            }

            // 查找最后一个分隔符
            size_t pos = path.find_last_of("/\\");
            if (pos == std::string_view::npos)
            {
                return std::string(path);
            }

            // 检查是否以分隔符结尾
            if (pos == path.size() - 1)
            {
                return "";
            }

            return std::string(path.substr(pos + 1));
        }

        // 获取文件名不带扩展名
        std::string getStem(std::string_view path)
        {
            std::string filename = getFileName(path);

            // 查找最后一个点
            size_t pos = filename.find_last_of('.');
            if (pos == std::string::npos)
            {
                return filename;
            }

            // 点在开头的情况
            if (pos == 0)
            {
                if (filename.size() == 1)
                {
                    return "";
                }
                // 检查是否是隐藏文件（只有一个点）
                if (filename == ".")
                {
                    return "";
                }
                // 对于 .hidden，返回 ""
                // 对于 .hidden.txt，返回 .hidden
                // 我们需要检查是否还有其他点
                size_t nextDot = filename.find('.', 1);
                if (nextDot == std::string::npos)
                {
                    return "";
                }
                return filename.substr(0, nextDot);
            }

            return filename.substr(0, pos);
        }

        // 获取扩展名
        std::string getExtension(std::string_view path)
        {
            std::string filename = getFileName(path);

            // 查找最后一个点
            size_t pos = filename.find_last_of('.');
            if (pos == std::string::npos)
            {
                return "";
            }

            // 点在开头的情况
            if (pos == 0)
            {
                if (filename.size() == 1)
                {
                    return "";
                }
                // 对于 .hidden，返回 ""
                // 对于 .hidden.txt，返回 txt
                size_t nextDot = filename.find('.', 1);
                if (nextDot == std::string::npos)
                {
                    return "";
                }
                return filename.substr(nextDot + 1);
            }

            return filename.substr(pos + 1);
        }

        // 获取父路径
        std::string getParentPath(std::string_view path)
        {
            if (path.empty())
            {
                return "";
            }

            // 去除尾部空格和分隔符
            size_t end = path.find_last_not_of("/\\");
            if (end == std::string_view::npos)
            {
                return "";
            }

            // 查找最后一个分隔符
            size_t pos = path.find_last_of("/\\", end);
            if (pos == std::string_view::npos)
            {
                return "";
            }

            // 再次去除尾部空格和分隔符
            size_t parentEnd = path.find_last_not_of("/\\", pos);
            if (parentEnd == std::string_view::npos)
            {
                return "";
            }

            return std::string(path.substr(0, parentEnd + 1));
        }

        // 判断是否为绝对路径
        bool isAbsolute(std::string_view path)
        {
            return Detail::isAbsoluteImpl(path);
        }

        // 判断是否有指定扩展名
        bool hasExtension(std::string_view path, std::string_view ext)
        {
            std::string extension = getExtension(path);

            if (ext.empty())
            {
                return extension.empty();
            }

            // 如果 ext 以 . 开头，去掉它
            if (ext[0] == '.')
            {
                ext = ext.substr(1);
            }

            return Detail::equalsIgnoreCase(extension, ext);
        }
    }
}
