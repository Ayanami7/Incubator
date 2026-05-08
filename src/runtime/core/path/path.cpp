#include "runtime/core/path/path.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace Incubator
{
    namespace Path
    {
        // 路径连接
        std::string join(std::string_view a, std::string_view b)
        {
            if (b.empty())
            {
                return std::string(a);
            }
            return (std::filesystem::path(a) / std::filesystem::path(b)).generic_string();
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

            // MSVC 在 generic 格式下可能丢失 //server 的 UNC root-name 语义。
            // 用 native 格式做 lexically_normal，再转换为 generic 格式。
            auto normalized = std::filesystem::path(path).lexically_normal().string();

            // 统一分隔符为 /
            std::string result;
            result.reserve(normalized.size());
            for (char c : normalized)
            {
                result += (c == '\\') ? '/' : c;
            }

            // 恢复 UNC 双斜杠前缀（原始路径以 // 开头时）
            if (path.size() >= 2 && path[0] == '/' && path[1] == '/')
            {
                if (result.starts_with('/'))
                {
                    result = '/' + result;  // /server → //server
                }
                else
                {
                    result = "//" + result;
                }
            }

            // 去除尾部重复分隔符（保留单斜杠根路径）
            while (result.size() > 1 && result.back() == '/')
            {
                result.pop_back();
            }

            return result;
        }

        // 获取文件名
        std::string getFileName(std::string_view path)
        {
            return std::filesystem::path(path).filename().string();
        }

        // 获取文件名（不含扩展名）
        std::string getStem(std::string_view path)
        {
            return std::filesystem::path(path).stem().string();
        }

        // 获取扩展名
        std::string getExtension(std::string_view path)
        {
            auto ext = std::filesystem::path(path).extension().string();
            if (!ext.empty() && ext[0] == '.')
            {
                ext = ext.substr(1);
            }
            return ext;
        }

        // 获取父路径
        std::string getParentPath(std::string_view path)
        {
            if (path.empty())
            {
                return "";
            }
            // 保留原行为：尾部分隔符 → 无父路径
            if (path.back() == '/' || path.back() == '\\')
            {
                return "";
            }
            return std::filesystem::path(path).parent_path().string();
        }

        // 判断是否为绝对路径
        bool isAbsolute(std::string_view path)
        {
            if (path.empty())
            {
                return false;
            }
            // MSVC 在 generic 格式下不识别 //server 的 UNC 前缀，
            // 需要手动检测（Windows UNC 路径以 // 或 \\ 开头）
            if (path.size() >= 2 && path[0] == '/' && path[1] == '/')
            {
                return true;
            }
            return std::filesystem::path(path).is_absolute();
        }

        // 判断扩展名是否匹配（大小写不敏感）
        bool hasExtension(std::string_view path, std::string_view ext)
        {
            auto extStr = std::filesystem::path(path).extension().string();
            std::string_view actual = extStr;
            if (!actual.empty() && actual[0] == '.')
            {
                actual = actual.substr(1);
            }

            if (!ext.empty() && ext[0] == '.')
            {
                ext = ext.substr(1);
            }

            if (actual.size() != ext.size())
            {
                return false;
            }

            return std::equal(actual.begin(), actual.end(), ext.begin(),
                              [](char a, char b) {
                                  return std::tolower(static_cast<unsigned char>(a)) ==
                                         std::tolower(static_cast<unsigned char>(b));
                              });
        }
    }  // namespace Path
}  // namespace Incubator
