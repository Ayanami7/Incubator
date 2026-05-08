#include "runtime/core/path/path.h"

#include <algorithm>
#include <filesystem>

namespace Incubator
{
    namespace Path
    {
        namespace
        {
            /// @brief 将 string_view 按 UTF-8 编码构造为 filesystem::path
            ///
            /// MSVC 上 std::filesystem::path(const char*) 使用系统代码页解释输入，
            /// 即使设置了 /utf-8 编译选项也是如此。只有通过 char8_t 构造函数才能保证
            /// 始终按 UTF-8 编码解释输入字符串。
            std::filesystem::path toPath(std::string_view sv)
            {
                return std::filesystem::path(
                    std::u8string(reinterpret_cast<const char8_t*>(sv.data()), sv.size()));
            }

            /// @brief 将 u8string 转换为 std::string（保留 UTF-8 字节序列）
            std::string fromU8String(const std::u8string& s)
            {
                return std::string(reinterpret_cast<const char*>(s.data()), s.size());
            }
        }  // anonymous namespace

        // ============================================================
        //  路径连接
        // ============================================================

        std::string join(std::string_view a, std::string_view b)
        {
            if (b.empty())
            {
                return std::string(a);
            }
            auto joined = (toPath(a) / toPath(b)).generic_u8string();
            return fromU8String(joined);
        }

        std::string join(std::string_view a, std::string_view b, std::string_view c)
        {
            return join(join(a, b), c);
        }

        // ============================================================
        //  路径规范化
        // ============================================================

        std::string normalize(std::string_view path)
        {
            if (path.empty())
            {
                return "";
            }

            // lexically_normal 用 native 格式处理以保留 UNC root-name 语义，
            // 输出则用 generic_u8string 保证 UTF-8 编码和 '/' 分隔符。
            auto normalized_u8 = toPath(path).lexically_normal().generic_u8string();
            std::string result = fromU8String(normalized_u8);

            // 恢复 UNC 双斜杠前缀（MSVC generic 格式可能丢失 //server 的 '//'）
            if (path.size() >= 2 && path[0] == '/' && path[1] == '/')
            {
                if (result.starts_with("//"))
                {
                    // 已经是双斜杠，lexically_normal 已保留 UNC 前缀，无需恢复
                }
                else if (result.starts_with('/'))
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

        // ============================================================
        //  组件提取
        // ============================================================

        std::string getFileName(std::string_view path)
        {
            auto s = toPath(path).filename().u8string();
            return fromU8String(s);
        }

        std::string getStem(std::string_view path)
        {
            auto s = toPath(path).stem().u8string();
            return fromU8String(s);
        }

        std::string getExtension(std::string_view path)
        {
            auto ext_u8 = toPath(path).extension().u8string();
            std::string ext = fromU8String(ext_u8);
            if (!ext.empty() && ext[0] == '.')
            {
                ext = ext.substr(1);
            }
            return ext;
        }

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
            auto parent_u8 = toPath(path).parent_path().generic_u8string();
            return fromU8String(parent_u8);
        }

        // ============================================================
        //  路径判断
        // ============================================================

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
            return toPath(path).is_absolute();
        }

        bool hasExtension(std::string_view path, std::string_view ext)
        {
            auto extStr_u8 = toPath(path).extension().u8string();
            std::string extStr = fromU8String(extStr_u8);
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

            // ASCII-only 大小写比较：安全处理非 ASCII 字符
            // （扩展名均为 ASCII，但此函数不应对非 ASCII 输入产生 UB）
            return std::equal(actual.begin(), actual.end(), ext.begin(),
                              [](char a, char b) {
                                  auto asciiToLower = [](char c) -> char {
                                      return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
                                  };
                                  return asciiToLower(a) == asciiToLower(b);
                              });
        }
    }  // namespace Path
}  // namespace Incubator
