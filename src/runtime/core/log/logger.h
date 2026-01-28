// logger.h
#pragma once

#include <spdlog/fmt/fmt.h>
#include <spdlog/logger.h>

#include <memory>
#include <string_view>
#include <utility>

#include "runtime/core/error/error.h"

namespace Incubator
{
    enum class LogLevel
    {
        TRACE,
        DEBUG,
        INFO,
        WARN,
        ERROR,
        CRITICAL,
        OFF
    };

    class Logger
    {
    public:
        // 全局初始化（配置所有 logger 的共享 sinks）
        static void init(LogLevel default_level = LogLevel::INFO,
                         std::string_view pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%-8l%$] [%-10n] %v");

        static Logger create(std::string_view module_name);

        static void setGlobalLevel(LogLevel level);

        static void flushAll();

        void setLevel(LogLevel level);
        LogLevel getLevel() const;

        void addSink(std::shared_ptr<spdlog::sinks::sink> sink);

        // 日志输出方法
        template <typename... Args>
        void trace(fmt::format_string<Args...> fmt, Args&&... args)
        {
            if (logger)
                logger->trace(fmt, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void debug(fmt::format_string<Args...> fmt, Args&&... args)
        {
            if (logger)
                logger->debug(fmt, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void info(fmt::format_string<Args...> fmt, Args&&... args)
        {
            if (logger)
                logger->info(fmt, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void warn(fmt::format_string<Args...> fmt, Args&&... args)
        {
            if (logger)
                logger->warn(fmt, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void error(fmt::format_string<Args...> fmt, Args&&... args)
        {
            if (logger)
                logger->error(fmt, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void critical(fmt::format_string<Args...> fmt, Args&&... args)
        {
            if (logger)
                logger->critical(fmt, std::forward<Args>(args)...);
        }

    private:
        // 私有化构造函数，通过 Create 创建
        explicit Logger(std::shared_ptr<spdlog::logger> logger);

        std::shared_ptr<spdlog::logger> logger;  // 每个实例持有 spdlog logger
    };

    void logError(Logger& logger, const Exception& err, LogLevel level);
}  // namespace Incubator