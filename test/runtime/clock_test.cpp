#define TESTING_ENABLED

#include "runtime/core/clock/clock.h"

#include <gtest/gtest.h>

#include <thread>

using namespace Incubator::Clock;

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
