#include "startup_config.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <span>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"

namespace Incubator::App
{
    namespace
    {
        [[noreturn]] void invalid(std::string_view field, std::string_view reason)
        {
            throw Error::Exception(Error::Code::CONFIG, fmt::format("{}: {}", field, reason));
        }

        std::string text(const rapidjson::Value& value, std::string_view field)
        {
            if (!value.IsString())
                invalid(field, "expected string");
            std::string result(value.GetString(), value.GetStringLength());
            if (result.empty() || result.find('\0') != std::string::npos)
                invalid(field, "expected non-empty string without NUL");
            return result;
        }

        int dimension(const rapidjson::Value& value, std::string_view field)
        {
            if (!value.IsInt() || value.GetInt() <= 0)
                invalid(field, "expected positive 32-bit integer");
            return value.GetInt();
        }
    }  // namespace

    StartupConfig::StartupConfig()
    {
        options.window.title = "Incubator Engine Demo";
    }

    StartupConfig decodeStartupConfig(std::string_view json)
    {
        if (json.find('\0') != std::string_view::npos)
            invalid("JSON", "unexpected NUL byte");
        rapidjson::Document document;
        document.Parse<rapidjson::kParseValidateEncodingFlag>(json.data(), json.size());
        if (document.HasParseError())
            invalid("JSON", fmt::format("{} at byte {}", rapidjson::GetParseError_En(document.GetParseError()),
                                        document.GetErrorOffset()));
        if (!document.IsObject())
            invalid("JSON", "expected object");
        StartupConfig config;
        if (const auto window = document.FindMember("window"); window != document.MemberEnd())
        {
            if (!window->value.IsObject())
                invalid("window", "expected object");
            const auto& object = window->value;
            if (const auto title = object.FindMember("title"); title != object.MemberEnd())
                config.options.window.title = text(title->value, "window.title");
            if (const auto width = object.FindMember("width"); width != object.MemberEnd())
                config.options.window.width = dimension(width->value, "window.width");
            if (const auto height = object.FindMember("height"); height != object.MemberEnd())
                config.options.window.height = dimension(height->value, "window.height");
        }
        if (const auto scene = document.FindMember("startupScene"); scene != document.MemberEnd())
            config.startupScene = text(scene->value, "startupScene");
        return config;
    }

    StartupConfig readStartupConfig(FileIO::FileIO& fileIO, std::string_view path)
    {
        if (path.empty() || path.find('\0') != std::string_view::npos || !Utils::Utf8::isValid(path))
            throw Error::Exception(Error::Code::IO, "Invalid UTF-8 startup config path");
        try
        {
            auto file = fileIO.open(Utils::Utf8::toFsPath(path), FileIO::OpenMode::Read);
            if (!file)
            {
                Log::Registry::get("app").debug("Startup config '{}' not found; using defaults", path);
                return {};
            }
            const auto size = file->size();
            if (size > std::string{}.max_size())
                throw Error::Exception(Error::Code::IO, "Startup config is too large");
            std::string json(static_cast<size_t>(size), '\0');
            size_t position = 0;
            while (position < json.size())
            {
                const auto count = file->read(std::span(reinterpret_cast<uint8_t*>(json.data() + position),
                                                       json.size() - position));
                if (!count)
                    throw Error::Exception(Error::Code::IO, "Incomplete startup config read");
                position += count;
            }
            auto config = decodeStartupConfig(json);
            Log::Registry::get("app").info("Loaded startup config: {}", path);
            return config;
        }
        catch (const Error::Exception& error)
        {
            throw Error::Exception(error.code(), fmt::format("Startup config '{}': {}", path, error.message()));
        }
    }
}  // namespace Incubator::App
