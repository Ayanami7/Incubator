#include "runtime/core/log/logger.h"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <mutex>
#include <unordered_map>
#include <vector>

namespace Incubator
{
    namespace
    {
        struct LogSystemState
        {
            std::mutex mutex;
            std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> loggers;
            std::vector<spdlog::sink_ptr> sinks;
            LogLevel default_level = LogLevel::INFO;
            bool initialized = false;
        };

        LogSystemState& state()
        {
            static LogSystemState instance;
            return instance;
        }

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

        LogLevel fromSpdLevel(spdlog::level::level_enum level)
        {
            switch (level)
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

    // LogSystem 实现
    void LogSystem::init(LogLevel default_level, std::string_view pattern)
    {
        std::lock_guard<std::mutex> lock(state().mutex);

        auto& s = state();
        // 1. 防止重复初始化：如果已经初始化过，不再重新创建 Sink
        if (s.initialized)
        {
            return;
        }

        s.default_level = default_level;

        // 2. 创建默认 Sink
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

        // 如果 pattern 为空，使用默认格式
        if (pattern.empty())
        {
            console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%-8l%$] [%-10n] %v");
        }
        else
        {
            console_sink->set_pattern(std::string{pattern});
        }

        s.sinks.push_back(console_sink);
        s.initialized = true;
    }

    // 获取指定模块的 logger
    Logger LogSystem::get(std::string_view module_name)
    {
        std::string name(module_name);
        auto& s = state();

        {
            // 第一次锁定：检查初始化和查找既有 Logger
            std::lock_guard<std::mutex> lock(s.mutex);

            // 1. 自动初始化逻辑：如果用户没调 init，则按默认参数初始化
            if (!s.initialized)
            {
                // 注意：因为 lock 已经是 state().mutex，这里不能直接调用 LogSystem::init(会死锁)
                // 所以我们将初始化最核心的逻辑在此内联，确保逻辑一致
                s.default_level = LogLevel::INFO;
                auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%-8l%$] [%-10n] %v");
                s.sinks.push_back(console_sink);
                s.initialized = true;
            }

            // 2. 查找缓存
            auto it = s.loggers.find(name);
            if (it != s.loggers.end())
            {
                return Logger(it->second);
            }

            // 3. 创建新 Logger
            // 这里复制一份当前的 sinks，防止在创建过程中 s.sinks 被其他操作修改导致迭代器失效
            auto current_sinks = s.sinks;
            auto new_spd_logger = std::make_shared<spdlog::logger>(name, current_sinks.begin(), current_sinks.end());

            new_spd_logger->set_level(toSpdLevel(s.default_level));
            new_spd_logger->flush_on(spdlog::level::err);

            s.loggers[name] = new_spd_logger;
            return Logger(new_spd_logger);
        }
    }

    void LogSystem::setGlobalLevel(LogLevel level)
    {
        std::lock_guard<std::mutex> lock(state().mutex);

        state().default_level = level;

        for (auto& [name, logger] : state().loggers)
        {
            logger->set_level(toSpdLevel(level));
        }
    }

    void LogSystem::flushAll()
    {
        std::lock_guard<std::mutex> lock(state().mutex);

        for (auto& [name, logger] : state().loggers)
        {
            logger->flush();
        }
    }

    // 重置日志系统
    void LogSystem::reset()
    {
        std::lock_guard<std::mutex> lock(state().mutex);

        auto& s = state();
        s.loggers.clear();
        s.sinks.clear();
        s.default_level = LogLevel::INFO;
        s.initialized = false;
    }

    // Logger 实现
    Logger::Logger(std::shared_ptr<spdlog::logger> logger) : logger_(std::move(logger)) {}

    void Logger::setLevel(LogLevel level)
    {
        if (logger_)
        {
            logger_->set_level(toSpdLevel(level));
        }
    }

    LogLevel Logger::getLevel() const
    {
        if (logger_)
        {
            return fromSpdLevel(logger_->level());
        }
        return LogLevel::OFF;
    }

    void Logger::addSink(std::shared_ptr<spdlog::sinks::sink> sink)
    {
        if (logger_ && sink)
        {
            logger_->sinks().push_back(std::move(sink));
        }
    }

    void logError(Logger& logger, const Exception& err, LogLevel level)
    {
        logWithLevel(logger, level, err.what());
    }
}  // namespace Incubator
