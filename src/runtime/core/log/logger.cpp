#include "runtime/core/log/logger.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <mutex>
#include <unordered_map>
#include <vector>

namespace Incubator
{
    // 全局状态（线程安全）
    std::mutex g_mutex;
    std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> g_loggers;
    std::vector<spdlog::sink_ptr> g_sinks;
    LogLevel g_default_level = LogLevel::INFO;
    bool g_initialized = false;

    namespace
    {
        // 统一内部日志分发，供 logError 复用，放入匿名命名空间避免符号外泄
        void logWithLevel(Logger& logger, LogLevel level, std::string_view msg)
        {
            switch (level)
            {
                case LogLevel::TRACE:
                    logger.trace("{}", msg);
                    break;
                case LogLevel::DEBUG:
                    logger.debug("{}", msg);
                    break;
                case LogLevel::INFO:
                    logger.info("{}", msg);
                    break;
                case LogLevel::WARN:
                    logger.warn("{}", msg);
                    break;
                case LogLevel::ERROR:
                    logger.error("{}", msg);
                    break;
                case LogLevel::CRITICAL:
                    logger.critical("{}", msg);
                    break;
                case LogLevel::OFF:
                    break;
                default:
                    logger.error("{}", msg);
                    break;
            }
        }
    }  // namespace

    // 内部转换函数
    spdlog::level::level_enum toSpdLevel(LogLevel level)
    {
        switch (level)
        {
            case LogLevel::TRACE:
                return spdlog::level::trace;
            case LogLevel::DEBUG:
                return spdlog::level::debug;
            case LogLevel::INFO:
                return spdlog::level::info;
            case LogLevel::WARN:
                return spdlog::level::warn;
            case LogLevel::ERROR:
                return spdlog::level::err;
            case LogLevel::CRITICAL:
                return spdlog::level::critical;
            case LogLevel::OFF:
                return spdlog::level::off;
            default:
                return spdlog::level::info;
        }
    }

    // 确保初始化（内部使用，调用前需持有锁）
    void ensureInitializedLocked()
    {
        if (g_initialized)
            return;

        // 创建共享的 sink（控制台彩色输出，多线程安全）
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%-8l%$] [%-10n] %v");
        g_sinks.push_back(console_sink);

        g_initialized = true;
    }

    // 获取或创建 logger（内部使用）
    std::shared_ptr<spdlog::logger> getOrCreateLogger(std::string_view module_name)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        ensureInitializedLocked();

        std::string name(module_name);
        auto it = g_loggers.find(name);
        if (it != g_loggers.end())
        {
            return it->second;
        }

        // 创建新的 logger，共享所有 sinks（多线程安全）
        auto logger = std::make_shared<spdlog::logger>(name, g_sinks.begin(), g_sinks.end());
        logger->set_level(toSpdLevel(g_default_level));
        logger->flush_on(spdlog::level::err);  // 错误级别自动刷新

        g_loggers[name] = logger;
        return logger;
    }

    // logger 初始化
    void Logger::init(LogLevel default_level, std::string_view pattern)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (g_initialized)
        {
            return;  // 已初始化，避免重复
        }

        g_default_level = default_level;

        // 创建共享的 sink（控制台彩色输出，多线程安全）
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern(std::string{pattern});
        g_sinks.push_back(console_sink);

        g_initialized = true;
    }

    Logger Logger::create(std::string_view module_name)
    {
        return Logger(getOrCreateLogger(module_name));
    }

    void Logger::setGlobalLevel(LogLevel level)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_default_level = level;

        // 更新所有已存在的 logger
        for (auto& [name, logger] : g_loggers)
        {
            logger->set_level(toSpdLevel(level));
        }
    }

    // 刷新所有 logger
    void Logger::flushAll()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& [name, logger] : g_loggers)
        {
            logger->flush();
        }
    }

    Logger::Logger(std::shared_ptr<spdlog::logger> logger) : logger(std::move(logger)) {}

    void Logger::setLevel(LogLevel level)
    {
        if (logger)
        {
            logger->set_level(toSpdLevel(level));
        }
    }

    LogLevel Logger::getLevel() const
    {
        if (logger)
        {
            auto spd_level = logger->level();
            switch (spd_level)
            {
                case spdlog::level::trace:
                    return LogLevel::TRACE;
                case spdlog::level::debug:
                    return LogLevel::DEBUG;
                case spdlog::level::info:
                    return LogLevel::INFO;
                case spdlog::level::warn:
                    return LogLevel::WARN;
                case spdlog::level::err:
                    return LogLevel::ERROR;
                case spdlog::level::critical:
                    return LogLevel::CRITICAL;
                case spdlog::level::off:
                    return LogLevel::OFF;
                default:
                    return LogLevel::INFO;
            }
        }
        return LogLevel::OFF;  // 默认返回 OFF 如果 logger 不存在
    }

    void Logger::addSink(std::shared_ptr<spdlog::sinks::sink> sink)
    {
        if (logger && sink)
        {
            logger->sinks().push_back(std::move(sink));
        }
    }

    void logError(Logger& logger, const Exception& err, LogLevel level)
    {
        // 复用 Exception 的格式化字符串输出到指定日志级别
        logWithLevel(logger, level, err.what());
    }
}  // namespace Incubator
