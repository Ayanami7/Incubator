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
        /// @brief 日志级别枚举
        ///
        /// 定义日志输出的详细程度，级别由低到高依次变高。
        enum class Level
        {
            TRACE,     ///< 最详细的调试信息（仅开发期使用）
            DEBUG,     ///< 调试信息
            INFO,      ///< 一般信息（默认级别）
            WARN,      ///< 警告信息
            ERROR,     ///< 错误信息
            CRITICAL,  ///< 严重错误
            OFF        ///< 关闭所有日志
        };

        class Logger;

        /// @brief 日志注册表（全局单例）
        ///
        /// 管理所有 logger 的共享状态（sinks、级别、缓存）。
        /// 所有静态方法都是线程安全的。
        class Registry
        {
        public:
            /// @brief 全局初始化
            /// @param default_level 默认日志级别（默认 INFO）
            /// @param pattern 日志输出格式（默认包含时间、级别、模块名）
            static void init(Level default_level = Level::INFO,
                             std::string_view pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%-8l%$] [%-10n] %v");

            /// @brief 获取指定模块的 Logger
            /// @param module_name 模块名称（相同名称返回同一实例）
            /// @return Logger 句柄（值类型，拷贝开销低）
            static Logger get(std::string_view module_name);

            /// @brief 设置所有 logger 的全局级别
            /// @param level 目标日志级别
            static void setGlobalLevel(Level level);

            /// @brief 刷新所有日志输出到 sink
            static void flushAll();

            /// @brief 重置日志系统（清理所有 logger 和 sink）
            static void reset();
        };

        /// @brief 日志句柄（轻量值类型）
        ///
        /// 各模块持有的日志输出接口，内部持 shared_ptr<spdlog::logger>。
        /// 支持 fmt 格式化风格的参数化日志输出。
        class Logger
        {
        public:
            Logger() = default;

            /// @brief 设置此 logger 的日志级别
            void setLevel(Level level);

            /// @brief 获取此 logger 的当前日志级别
            Level getLevel() const;

            /// @brief 添加自定义 sink
            /// @param sink spdlog sink（如文件 sink、网络 sink 等）
            void addSink(std::shared_ptr<spdlog::sinks::sink> sink);

            /// @brief 输出 TRACE 级别日志
            template <typename... Args>
            void trace(fmt::format_string<Args...> fmt, Args&&... args)
            {
                if (logger_)
                    logger_->trace(fmt, std::forward<Args>(args)...);
            }

            /// @brief 输出 DEBUG 级别日志
            template <typename... Args>
            void debug(fmt::format_string<Args...> fmt, Args&&... args)
            {
                if (logger_)
                    logger_->debug(fmt, std::forward<Args>(args)...);
            }

            /// @brief 输出 INFO 级别日志
            template <typename... Args>
            void info(fmt::format_string<Args...> fmt, Args&&... args)
            {
                if (logger_)
                    logger_->info(fmt, std::forward<Args>(args)...);
            }

            /// @brief 输出 WARN 级别日志
            template <typename... Args>
            void warn(fmt::format_string<Args...> fmt, Args&&... args)
            {
                if (logger_)
                    logger_->warn(fmt, std::forward<Args>(args)...);
            }

            /// @brief 输出 ERROR 级别日志
            template <typename... Args>
            void error(fmt::format_string<Args...> fmt, Args&&... args)
            {
                if (logger_)
                    logger_->error(fmt, std::forward<Args>(args)...);
            }

            /// @brief 输出 CRITICAL 级别日志
            template <typename... Args>
            void critical(fmt::format_string<Args...> fmt, Args&&... args)
            {
                if (logger_)
                    logger_->critical(fmt, std::forward<Args>(args)...);
            }

        private:
            friend class Registry;
            explicit Logger(std::shared_ptr<spdlog::logger> logger);

            std::shared_ptr<spdlog::logger> logger_;
        };

        /// @brief 将 Exception 输出到日志
        /// @param logger 目标 Logger
        /// @param err 异常对象
        /// @param level 日志级别
        void logError(Logger& logger, const Error::Exception& err, Level level);
    }  // namespace Log
}  // namespace Incubator
