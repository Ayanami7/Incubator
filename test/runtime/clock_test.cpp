#define TESTING_ENABLED

#include "runtime/core/clock/clock.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <thread>

using namespace Incubator;

class ClockTest : public ::testing::Test
{};

TEST_F(ClockTest, NowReturnsValidTimePoint)
{
    TimePoint t1 = now();
    EXPECT_NE(t1.time_since_epoch().count(), 0);

    TimePoint t2 = now();
    EXPECT_NE(t2.time_since_epoch().count(), 0);

    EXPECT_GE(t2, t1);
}

TEST_F(ClockTest, ElapsedSinceReturnsNonNegativeDuration)
{
    TimePoint start = now();

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    Duration elapsed = elapsedSince(start);
    EXPECT_GE(elapsed.count(), 0);
}

TEST_F(ClockTest, ElapsedSecondsReturnsApproximatelyCorrectValue)
{
    TimePoint start = now();

    const double expectedSleep = 0.05;
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int64_t>(expectedSleep * 1000)));

    double elapsed = elapsedSeconds(start);
    EXPECT_GE(elapsed, expectedSleep);
    EXPECT_LE(elapsed, expectedSleep + 0.1);
}

TEST_F(ClockTest, ElapsedMillisecondsReturnsApproximatelyCorrectValue)
{
    TimePoint start = now();

    const int64_t expectedSleep = 50;
    std::this_thread::sleep_for(std::chrono::milliseconds(expectedSleep));

    int64_t elapsed = elapsedMilliseconds(start);
    EXPECT_GE(elapsed, expectedSleep);
    EXPECT_LE(elapsed, expectedSleep + 20);
}

TEST_F(ClockTest, TimeMonotonicallyIncreases)
{
    TimePoint t1 = now();
    TimePoint t2 = now();
    TimePoint t3 = now();

    EXPECT_LE(t1, t2);
    EXPECT_LE(t2, t3);
}

TEST_F(ClockTest, FastCallHasMinimalOverhead)
{
    constexpr int iterations = 100000;

    auto startTime = now();

    for (int i = 0; i < iterations; ++i)
    {
        volatile auto _ = now();
        (void)_;
    }

    auto endTime = now();

    Duration totalDuration = endTime - startTime;
    double totalTime = std::chrono::duration<double>(totalDuration).count();
    double avgTime = totalTime / iterations;

    EXPECT_LT(avgTime, 0.000001);
}

TEST_F(ClockTest, ConcurrentCallsAreThreadSafe)
{
    constexpr int threads = 10;
    constexpr int callsPerThread = 1000;

    std::vector<std::thread> workers;
    std::atomic<int> successCount{0};

    for (int i = 0; i < threads; ++i)
    {
        workers.emplace_back([&] {
            try
            {
                for (int j = 0; j < callsPerThread; ++j)
                {
                    TimePoint t = now();
                    double s = elapsedSeconds(t);
                    int64_t ms = elapsedMilliseconds(t);
                    (void)t;
                    (void)s;
                    (void)ms;
                }
                successCount++;
            }
            catch (...)
            {}
        });
    }

    for (auto& t : workers)
        t.join();

    EXPECT_EQ(successCount, threads);
}
