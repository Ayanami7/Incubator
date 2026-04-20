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

    // Logger 轻量句柄，各模块持有使用
    class Logger;

    // 全局日志系统管理器
    class LogSystem
    {
    public:
        // 全局初始化（配置所有 logger 的共享 sinks）
        static void init(LogLevel default_level = LogLevel::INFO,
                         std::string_view pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%-8l%$] [%-10n] %v");

        // 获取指定模块的 logger
        static Logger get(std::string_view module_name);

        // 设置全局日志级别
        static void setGlobalLevel(LogLevel level);

        // 刷新所有 logger
        static void flushAll();

        // 重置日志系统（仅用于测试）
        static void reset();
    };

    // Logger 轻量句柄，各模块持有使用
    class Logger
    {
    public:
        Logger() = default;

        void setLevel(LogLevel level);
        LogLevel getLevel() const;

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
        friend class LogSystem;
    };

    void logError(Logger& logger, const Exception& err, LogLevel level);
}
