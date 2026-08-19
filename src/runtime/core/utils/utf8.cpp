#include "runtime/core/utils/utf8.h"

#include <utf8.h>
#include <utf8/cpp17.h>

namespace Incubator
{
    namespace Utils
    {
        namespace Utf8
        {
            bool isValid(std::string_view s)
            {
                return utf8::is_valid(s);
            }

            std::string toUtf8String(const std::filesystem::path& path)
            {
                auto u8 = path.u8string();
                return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
            }

            std::filesystem::path toFsPath(std::string_view utf8)
            {
                // MSVC 上 std::filesystem::path(const char*) 使用系统代码页解释输入，
                // 即使设置了 /utf-8 编译选项也是如此。只有通过 char8_t 构造函数才能保证
                // 始终按 UTF-8 编码解释输入字符串。
                return std::filesystem::path(
                    std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
            }
        }
    }
}