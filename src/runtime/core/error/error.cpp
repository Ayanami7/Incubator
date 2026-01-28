#include "runtime/core/error/error.h"

#include <string_view>

namespace Incubator
{
    // 将错误码转换为字符串
    std::string toString(ErrorCode code)
    {
        switch (code)
        {
            case ErrorCode::UNKNOWN:
                return "UNKNOWN";
            case ErrorCode::IO:
                return "IO";
            case ErrorCode::CONFIG:
                return "CONFIG";
            case ErrorCode::NETWORK:
                return "NETWORK";
            default:
                return "UNKNOWN";
        }
    }

    // 用于从路径中提取文件名
    std::string_view getFileName(std::string_view path)
    {
        auto pos = path.find_last_of("/\\");
        return (pos == std::string_view::npos) ? path : path.substr(pos + 1);
    }

    // Exception构造函数，初始化列表
    Exception::Exception(ErrorCode code, std::string message, std::source_location loc)
        : code_(code), message_(std::move(message)), location_(loc)
    {
        // 在构造时直接生成 what_cache_
        what_cache_ = fmt::format("[{}]  {}  @{}({}:{})", toString(code_), message_,
                                  getFileName(location_.file_name()),  // 只取文件名
                                  location_.line(), location_.function_name());
    }

    const char* Exception::what() const noexcept
    {
        return what_cache_.c_str();
    }
}  // namespace Incubator
