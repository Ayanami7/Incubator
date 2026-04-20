#include "runtime/core/log/logger.h"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <mutex>
#include <unordered_map>
#include <vector>

using Incubator::Log::Logger;

namespace Incubator
{
    namespace Log
    {
        namespace
        {
            struct RegistryState
            {
                std::mutex mutex;
                std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> loggers;
                std::vector<spdlog::sink_ptr> sinks;
                Level default_level = Level::INFO;
                bool initialized = false;
            };

            RegistryState& state()
            {
                static RegistryState instance;
                return instance;
            }

            spdlog::level::level_enum toSpdLevel(Level level)
            {
                switch (level)
                {
                    case Level::TRACE:
                        return spdlog::level::trace;
                    case Level::DEBUG:
                        return spdlog::level::debug;
                    case Level::INFO:
                        return spdlog::level::info;
                    case Level::WARN:
                        return spdlog::level::warn;
                    case Level::ERROR:
                        return spdlog::level::err;
                    case Level::CRITICAL:
                        return spdlog::level::critical;
                    case Level::OFF:
                        return spdlog::level::off;
                    default:
                        return spdlog::level::info;
                }
            }

            Level fromSpdLevel(spdlog::level::level_enum level)
            {
                switch (level)
                {
                    case spdlog::level::trace:
                        return Level::TRACE;
                    case spdlog::level::debug:
                        return Level::DEBUG;
                    case spdlog::level::info:
                        return Level::INFO;
                    case spdlog::level::warn:
                        return Level::WARN;
                    case spdlog::level::err:
                        return Level::ERROR;
                    case spdlog::level::critical:
                        return Level::CRITICAL;
                    case spdlog::level::off:
                        return Level::OFF;
                    default:
                        return Level::INFO;
                }
            }

            void logWithLevel(Logger& logger, Level level, std::string_view msg)
            {
                switch (level)
                {
                    case Level::TRACE:
                        logger.trace("{}", msg);
                        break;
                    case Level::DEBUG:
                        logger.debug("{}", msg);
                        break;
                    case Level::INFO:
                        logger.info("{}", msg);
                        break;
                    case Level::WARN:
                        logger.warn("{}", msg);
                        break;
                    case Level::ERROR:
                        logger.error("{}", msg);
                        break;
                    case Level::CRITICAL:
                        logger.critical("{}", msg);
                        break;
                    case Level::OFF:
                        break;
                    default:
                        logger.error("{}", msg);
                        break;
                }
            }
        }  // namespace

        void Registry::init(Level default_level, std::string_view pattern)
        {
            std::lock_guard<std::mutex> lock(state().mutex);

            auto& s = state();
            if (s.initialized)
            {
                return;
            }

            s.default_level = default_level;

            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

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

        Logger Registry::get(std::string_view module_name)
        {
            std::string name(module_name);
            auto& s = state();

            {
                std::lock_guard<std::mutex> lock(s.mutex);

                if (!s.initialized)
                {
                    s.default_level = Level::INFO;
                    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                    console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%-8l%$] [%-10n] %v");
                    s.sinks.push_back(console_sink);
                    s.initialized = true;
                }

                auto it = s.loggers.find(name);
                if (it != s.loggers.end())
                {
                    return Logger(it->second);
                }

                auto current_sinks = s.sinks;
                auto new_spd_logger =
                    std::make_shared<spdlog::logger>(name, current_sinks.begin(), current_sinks.end());

                new_spd_logger->set_level(toSpdLevel(s.default_level));
                new_spd_logger->flush_on(spdlog::level::err);

                s.loggers[name] = new_spd_logger;
                return Logger(new_spd_logger);
            }
        }

        void Registry::setGlobalLevel(Level level)
        {
            std::lock_guard<std::mutex> lock(state().mutex);

            state().default_level = level;

            for (auto& [name, logger] : state().loggers)
            {
                logger->set_level(toSpdLevel(level));
            }
        }

        void Registry::flushAll()
        {
            std::lock_guard<std::mutex> lock(state().mutex);

            for (auto& [name, logger] : state().loggers)
            {
                logger->flush();
            }
        }

        void Registry::reset()
        {
            std::lock_guard<std::mutex> lock(state().mutex);

            auto& s = state();
            s.loggers.clear();
            s.sinks.clear();
            s.default_level = Level::INFO;
            s.initialized = false;
        }

        Logger::Logger(std::shared_ptr<spdlog::logger> logger) : logger_(std::move(logger)) {}

        void Logger::setLevel(Level level)
        {
            if (logger_)
            {
                logger_->set_level(toSpdLevel(level));
            }
        }

        Level Logger::getLevel() const
        {
            if (logger_)
            {
                return fromSpdLevel(logger_->level());
            }
            return Level::OFF;
        }

        void Logger::addSink(std::shared_ptr<spdlog::sinks::sink> sink)
        {
            if (logger_ && sink)
            {
                logger_->sinks().push_back(std::move(sink));
            }
        }

        void logError(Logger& logger, const Error::Exception& err, Level level)
        {
            logWithLevel(logger, level, err.what());
        }
    }  // namespace Log
}  // namespace Incubator
