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

        std::string_view getFileName(std::string_view path)
        {
            auto pos = path.find_last_of("/\\");
            return (pos == std::string_view::npos) ? path : path.substr(pos + 1);
        }

        Exception::Exception(Code code, std::string message, std::source_location loc)
            : code_(code), message_(std::move(message)), location_(loc)
        {
            what_cache_ = fmt::format("[{}]  {}  @{}({}:{})", toString(code_), message_,
                                      getFileName(location_.file_name()), location_.line(), location_.function_name());
        }

        const char* Exception::what() const noexcept
        {
            return what_cache_.c_str();
        }
    }  // namespace Error
}  // namespace Incubator
