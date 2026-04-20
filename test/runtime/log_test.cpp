#define TESTING_ENABLED

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <sstream>
#include <thread>
#include <vector>

#include "runtime/core/log/logger.h"

using Incubator::LogLevel;
using Incubator::LogSystem;

class LoggerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        LogSystem::reset();
    }

    void TearDown() override
    {
        LogSystem::reset();
    }
};

TEST_F(LoggerTest, AutoInitializeWithoutExplicitInit)
{
    auto logger = LogSystem::get("auto.init");

    EXPECT_NO_THROW(logger.info("Auto initialized"));
    EXPECT_EQ(logger.getLevel(), LogLevel::INFO);
}

TEST_F(LoggerTest, InitAndBasicLoggingDoesNotThrow)
{
    LogSystem::init();
    auto logger = LogSystem::get("unit.logger");

    EXPECT_EQ(logger.getLevel(), LogLevel::INFO);

    EXPECT_NO_THROW(logger.trace("literal trace"));
    EXPECT_NO_THROW(logger.debug("value {}", 42));
    EXPECT_NO_THROW(logger.info(std::string_view{"runtime info"}));
    EXPECT_NO_THROW(logger.warn("warn {} {}", "two", 2));
    EXPECT_NO_THROW(logger.error("runtime error"));
    EXPECT_NO_THROW(logger.critical("fatal {}", 1));

    LogSystem::setGlobalLevel(LogLevel::WARN);
    EXPECT_EQ(logger.getLevel(), LogLevel::WARN);

    EXPECT_NO_THROW(LogSystem::flushAll());
}

TEST_F(LoggerTest, LogErrorUsesProvidedLevel)
{
    LogSystem::init();
    auto logger = LogSystem::get("unit.logger.error");

    std::ostringstream stream = std::ostringstream();
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(stream);

    sink->set_pattern("%l:%v");

    logger.addSink(sink);

    logger.setLevel(LogLevel::TRACE);

    Incubator::Exception ex(Incubator::ErrorCode::IO, "io failure");

    logError(logger, ex, LogLevel::ERROR);

    EXPECT_THAT(stream.str(), ::testing::HasSubstr("error"));
    EXPECT_THAT(stream.str(), ::testing::HasSubstr("io failure"));

    stream.str("");
    stream.clear();

    logError(logger, ex, LogLevel::INFO);
    EXPECT_THAT(stream.str(), ::testing::HasSubstr("info"));
}

TEST_F(LoggerTest, SameModuleNameReusesLogger)
{
    LogSystem::init();

    auto logger1 = LogSystem::get("shared.module");
    auto logger2 = LogSystem::get("shared.module");

    logger1.setLevel(LogLevel::TRACE);

    EXPECT_EQ(logger2.getLevel(), LogLevel::TRACE);
}

TEST_F(LoggerTest, DifferentModuleNamesCreateIndependentLoggers)
{
    LogSystem::init();

    auto loggerA = LogSystem::get("module.A");
    auto loggerB = LogSystem::get("module.B");

    loggerA.setLevel(LogLevel::ERROR);
    loggerB.setLevel(LogLevel::DEBUG);

    EXPECT_EQ(loggerA.getLevel(), LogLevel::ERROR);
    EXPECT_EQ(loggerB.getLevel(), LogLevel::DEBUG);
}

TEST_F(LoggerTest, ConcurrentGetIsStable)
{
    const int threads = 20;
    std::vector<std::thread> ts;

    std::atomic<bool> ok{true};

    for (int i = 0; i < threads; i++)
    {
        ts.emplace_back([&] {
            try
            {
                auto l1 = LogSystem::get("shared");
                auto l2 = LogSystem::get("shared");

                if (l1.getLevel() != l2.getLevel())
                    ok = false;
            }
            catch (...)
            {
                ok = false;
            }
        });
    }

    for (auto& t : ts)
        t.join();

    EXPECT_TRUE(ok);
}