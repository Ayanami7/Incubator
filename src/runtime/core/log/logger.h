#pragma once

#include <spdlog/fmt/fmt.h>
#include <spdlog/logger.h>

#include <memory>
#include <string_view>
#include <utility>

#include "runtime/core/error/error.h"

namespace Incubator
{
    namespace Log
    {
        enum class Level
        {
            TRACE,
            DEBUG,
            INFO,
            WARN,
            ERROR,
            CRITICAL,
            OFF
        };

        class Logger;

        class Registry
        {
        public:
            static void init(Level default_level = Level::INFO,
                             std::string_view pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%-8l%$] [%-10n] %v");

            static Logger get(std::string_view module_name);

            static void setGlobalLevel(Level level);

            static void flushAll();

            static void reset();
        };

        class Logger
        {
        public:
            Logger() = default;

            void setLevel(Level level);
            Level getLevel() const;

            void addSink(std::shared_ptr<spdlog::sinks::sink> sink);

            template <typename... Args>
            void trace(fmt::format_string<Args...> fmt, Args&&... args)
            {
                if (logger_)
                    logger_->trace(fmt, std::forward<Args>(args)...);
            }

            template <typename... Args>
            void debug(fmt::format_string<Args...> fmt, Args&&... args)
            {
                if (logger_)
                    logger_->debug(fmt, std::forward<Args>(args)...);
            }

            template <typename... Args>
            void info(fmt::format_string<Args...> fmt, Args&&... args)
            {
                if (logger_)
                    logger_->info(fmt, std::forward<Args>(args)...);
            }

            template <typename... Args>
            void warn(fmt::format_string<Args...> fmt, Args&&... args)
            {
                if (logger_)
                    logger_->warn(fmt, std::forward<Args>(args)...);
            }

            template <typename... Args>
            void error(fmt::format_string<Args...> fmt, Args&&... args)
            {
                if (logger_)
                    logger_->error(fmt, std::forward<Args>(args)...);
            }

            template <typename... Args>
            void critical(fmt::format_string<Args...> fmt, Args&&... args)
            {
                if (logger_)
                    logger_->critical(fmt, std::forward<Args>(args)...);
            }

        private:
            explicit Logger(std::shared_ptr<spdlog::logger> logger);

            std::shared_ptr<spdlog::logger> logger_;
            friend class Registry;
        };

        void logError(Logger& logger, const Error::Exception& err, Level level);
    }  // namespace Log

}  // namespace Incubator
