#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <sstream>

#include "runtime/core/log/logger.h"

using Incubator::Logger;
using Incubator::LogLevel;

TEST(LoggerTest, InitAndBasicLoggingDoesNotThrow)
{
    Logger::init();
    auto logger = Logger::create("unit.logger");

    EXPECT_EQ(logger.getLevel(), LogLevel::INFO);

    EXPECT_NO_THROW(logger.trace("literal trace"));
    EXPECT_NO_THROW(logger.debug("value {}", 42));
    EXPECT_NO_THROW(logger.info(std::string_view{"runtime info"}));
    EXPECT_NO_THROW(logger.warn("warn {} {}", "two", 2));
    EXPECT_NO_THROW(logger.error("runtime error"));
    EXPECT_NO_THROW(logger.critical("fatal {}", 1));

    Logger::setGlobalLevel(LogLevel::WARN);
    EXPECT_EQ(logger.getLevel(), LogLevel::WARN);

    EXPECT_NO_THROW(Logger::flushAll());
}

TEST(LoggerTest, LogErrorUsesProvidedLevel)
{
    Logger::init();
    auto logger = Logger::create("unit.logger.error");

    // 捕获日志输出的 stream
    std::ostringstream stream;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(stream);

    // 指定输出格式：级别 + : + 消息内容
    sink->set_pattern("%l:%v");

    // 把 sink 注册到 logger 内部
    logger.addSink(sink);

    // 设定最低输出 level 以确保所有测试级别都会输出
    logger.setLevel(LogLevel::TRACE);

    // 制造一个 Exception
    Incubator::Exception ex(Incubator::ErrorCode::IO, "io failure");

    // 调用 logError，在我们的 sink 中捕获输出
    logError(logger, ex, LogLevel::ERROR);

    // 确保 stderr 中有 ERROR: 和 io failure
    EXPECT_THAT(stream.str(), ::testing::HasSubstr("error"));
    EXPECT_THAT(stream.str(), ::testing::HasSubstr("io failure"));

    // 清空 stream 内容
    stream.str("");
    stream.clear();

    // 再一次以 INFO 级别记录
    logError(logger, ex, LogLevel::INFO);
    EXPECT_THAT(stream.str(), ::testing::HasSubstr("info"));
}
