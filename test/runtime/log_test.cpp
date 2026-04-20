#define TESTING_ENABLED

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <sstream>
#include <thread>
#include <vector>

#include "runtime/core/log/logger.h"

using Incubator::Log::Level;
using Incubator::Log::Registry;

class LoggerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Registry::reset();
    }

    void TearDown() override
    {
        Registry::reset();
    }
};

TEST_F(LoggerTest, AutoInitializeWithoutExplicitInit)
{
    auto logger = Registry::get("auto.init");

    EXPECT_NO_THROW(logger.info("Auto initialized"));
    EXPECT_EQ(logger.getLevel(), Level::INFO);
}

TEST_F(LoggerTest, InitAndBasicLoggingDoesNotThrow)
{
    Registry::init();
    auto logger = Registry::get("unit.logger");

    EXPECT_EQ(logger.getLevel(), Level::INFO);

    EXPECT_NO_THROW(logger.trace("literal trace"));
    EXPECT_NO_THROW(logger.debug("value {}", 42));
    EXPECT_NO_THROW(logger.info(std::string_view{"runtime info"}));
    EXPECT_NO_THROW(logger.warn("warn {} {}", "two", 2));
    EXPECT_NO_THROW(logger.error("runtime error"));
    EXPECT_NO_THROW(logger.critical("fatal {}", 1));

    Registry::setGlobalLevel(Level::WARN);
    EXPECT_EQ(logger.getLevel(), Level::WARN);

    EXPECT_NO_THROW(Registry::flushAll());
}

TEST_F(LoggerTest, LogErrorUsesProvidedLevel)
{
    Registry::init();
    auto logger = Registry::get("unit.logger.error");

    std::ostringstream stream = std::ostringstream();
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(stream);

    sink->set_pattern("%l:%v");

    logger.addSink(sink);

    logger.setLevel(Level::TRACE);

    Incubator::Error::Exception ex(Incubator::Error::Code::IO, "io failure");

    Incubator::Log::logError(logger, ex, Level::ERROR);

    EXPECT_THAT(stream.str(), ::testing::HasSubstr("error"));
    EXPECT_THAT(stream.str(), ::testing::HasSubstr("io failure"));

    stream.str("");
    stream.clear();

    Incubator::Log::logError(logger, ex, Level::INFO);
    EXPECT_THAT(stream.str(), ::testing::HasSubstr("info"));
}

TEST_F(LoggerTest, SameModuleNameReusesLogger)
{
    Registry::init();

    auto logger1 = Registry::get("shared.module");
    auto logger2 = Registry::get("shared.module");

    logger1.setLevel(Level::TRACE);

    EXPECT_EQ(logger2.getLevel(), Level::TRACE);
}

TEST_F(LoggerTest, DifferentModuleNamesCreateIndependentLoggers)
{
    Registry::init();

    auto loggerA = Registry::get("module.A");
    auto loggerB = Registry::get("module.B");

    loggerA.setLevel(Level::ERROR);
    loggerB.setLevel(Level::DEBUG);

    EXPECT_EQ(loggerA.getLevel(), Level::ERROR);
    EXPECT_EQ(loggerB.getLevel(), Level::DEBUG);
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
                auto l1 = Registry::get("shared");
                auto l2 = Registry::get("shared");

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