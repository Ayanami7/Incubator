#define TESTING_ENABLED

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>
#include <vector>

#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"

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

class LoggerFileTest : public LoggerTest
{
protected:
    void SetUp() override
    {
        LoggerTest::SetUp();
        directory_ = std::filesystem::temp_directory_path() /
                     ("incubator-log-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        config_.log_dir = Incubator::Utils::Utf8::toUtf8String(directory_);
    }

    void TearDown() override
    {
        LoggerTest::TearDown();
        std::filesystem::remove_all(directory_);
    }

    std::string readLog(const std::filesystem::path& name = "app.log")
    {
        std::ifstream input(directory_ / name, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    std::filesystem::path directory_;
    Incubator::Log::FileSinkConfig config_;
};

TEST_F(LoggerFileTest, FileSinkHotplugAppliesToExistingAndNewLoggers)
{
    auto existing = Registry::get("existing");
    Registry::setFileSinkConfig(config_);
    existing.info("existing enabled");
    Registry::get("new").info("new enabled");
    Registry::flushAll();
    Registry::setFileSinkConfig({});
    existing.info("disabled");
    Registry::flushAll();
    const auto contents = readLog();
    EXPECT_THAT(contents, ::testing::HasSubstr("existing enabled"));
    EXPECT_THAT(contents, ::testing::HasSubstr("new enabled"));
    EXPECT_THAT(contents, ::testing::Not(::testing::HasSubstr("disabled")));
}

TEST_F(LoggerFileTest, Utf8DirectoryAndBasenameSurviveFileCreationAndRotation)
{
    const auto logDirectory = directory_ / Incubator::Utils::Utf8::toFsPath("日志 空格-😀");
    config_.log_dir = Incubator::Utils::Utf8::toUtf8String(logDirectory);
    config_.basename = "引擎-🚀";
    config_.max_size = 128;
    config_.max_files = 2;
    Registry::init(Level::INFO, {}, config_);
    auto logger = Registry::get("unicode");
    for (int line = 0; line < 10; ++line)
        logger.info("中文内容-😀 {}", line);
    Registry::flushAll();
    Registry::setFileSinkConfig({});

    EXPECT_TRUE(std::filesystem::exists(logDirectory / Incubator::Utils::Utf8::toFsPath(config_.basename + ".log")));
    EXPECT_TRUE(std::filesystem::exists(logDirectory / Incubator::Utils::Utf8::toFsPath(config_.basename + ".1.log")));
    EXPECT_TRUE(std::filesystem::exists(logDirectory / Incubator::Utils::Utf8::toFsPath(config_.basename + ".2.log")));
    EXPECT_THAT(readLog(logDirectory / Incubator::Utils::Utf8::toFsPath(config_.basename + ".log")),
                ::testing::HasSubstr("中文内容-😀 9"));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory_), std::filesystem::directory_iterator()), 1);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(logDirectory), std::filesystem::directory_iterator()), 3);
}

TEST_F(LoggerFileTest, ConcurrentLoggingFlushAndSinkChangesPreserveMessagesAndIsolation)
{
    Registry::init();
    auto logger = Registry::get("concurrent");
    std::ostringstream captured;
    auto capture = std::make_shared<spdlog::sinks::ostream_sink_mt>(captured);
    capture->set_pattern("%v");
    logger.addSink(capture);
    std::ostringstream added;
    auto addedSink = std::make_shared<spdlog::sinks::ostream_sink_mt>(added);
    addedSink->set_pattern("%v");

    std::barrier start(5);
    std::vector<std::thread> workers;
    for (int thread = 0; thread < 4; ++thread)
        workers.emplace_back([&, thread] {
            auto copy = logger;
            start.arrive_and_wait();
            for (int message = 0; message < 64; ++message)
                copy.error("worker {} message {}", thread, message);  // error 同时触发 flush
        });
    std::thread changeSinks([&] {
        start.arrive_and_wait();
        logger.addSink(addedSink);
        for (int change = 0; change < 16; ++change)
        {
            Registry::setFileSinkConfig(config_);
            Registry::flushAll();
            Registry::setFileSinkConfig({});
        }
    });
    for (auto& worker : workers)
        worker.join();
    changeSinks.join();

    logger.info("after changes");
    Registry::get("other").info("other module");
    Registry::flushAll();
    const auto messages = captured.str();
    EXPECT_EQ(std::count(messages.begin(), messages.end(), '\n'), 257);
    EXPECT_THAT(added.str(), ::testing::HasSubstr("after changes"));
    EXPECT_THAT(added.str(), ::testing::Not(::testing::HasSubstr("other module")));
}
