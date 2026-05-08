#include "runtime/core/error/error.h"

#include <string_view>

using Incubator::Error::Code;

namespace Incubator
{
    namespace Error
    {
        std::string toString(Code code)
        {
            switch (code)
            {
                case Code::UNKNOWN:
                    return "UNKNOWN";
                case Code::IO:
                    return "IO";
                case Code::CONFIG:
                    return "CONFIG";
                case Code::NETWORK:
                    return "NETWORK";
                default:
                    return "UNKNOWN";
            }
        }

        Exception::Exception(Code code, std::string message, std::source_location loc)
            : code_(code), message_(std::move(message)), location_(loc)
        {
            // 从完整路径中提取文件名
            auto file = std::string_view(location_.file_name());
            auto pos = file.find_last_of("/\\");
            auto shortName = (pos == std::string_view::npos) ? file : file.substr(pos + 1);

            what_cache_ = fmt::format("[{}]  {}  @{}({}:{})", toString(code_), message_,
                                      shortName, location_.line(), location_.function_name());
        }

        const char* Exception::what() const noexcept
        {
            return what_cache_.c_str();
        }
    }  // namespace Error
}  // namespace Incubator
