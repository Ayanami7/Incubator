#pragma once

#include <chrono>

namespace Incubator
{
    namespace Clock
    {
        using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;
        using Duration = std::chrono::steady_clock::duration;

        inline TimePoint now()
        {
            return std::chrono::steady_clock::now();
        }

        inline Duration elapsedSince(TimePoint start)
        {
            return now() - start;
        }

        inline double elapsedSeconds(TimePoint start)
        {
            return std::chrono::duration<double>(elapsedSince(start)).count();
        }

        inline int64_t elapsedMilliseconds(TimePoint start)
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(elapsedSince(start)).count();
        }

        inline int64_t elapsedMicroseconds(TimePoint start)
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(elapsedSince(start)).count();
        }
    }
}
