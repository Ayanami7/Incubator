#pragma once

#include <chrono>

namespace Incubator
{
    using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;
    using Duration = std::chrono::steady_clock::duration;

    // 获取当前时间点
    inline TimePoint now()
    {
        return std::chrono::steady_clock::now();
    }

    // 计算从 start 到现在的持续时间
    inline Duration elapsedSince(TimePoint start)
    {
        return now() - start;
    }

    // 计算从 start 到现在的持续时间，单位为秒
    inline double elapsedSeconds(TimePoint start)
    {
        return std::chrono::duration<double>(elapsedSince(start)).count();
    }

    // 计算从 start 到现在的持续时间，单位为毫秒
    inline int64_t elapsedMilliseconds(TimePoint start)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(elapsedSince(start)).count();
    }
}  // namespace Incubator
