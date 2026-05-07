#pragma once

#include <chrono>

namespace Incubator
{
    namespace Clock
    {
        /// @brief 时间点类型，基于单调时钟
        using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;

        /// @brief 时间间隔类型
        using Duration = std::chrono::steady_clock::duration;

        /// @brief 获取当前时间点
        /// @return 从 steady_clock epoch 到当前的 TimePoint
        inline TimePoint now()
        {
            return std::chrono::steady_clock::now();
        }

        /// @brief 计算从 start 到现在经过的时间间隔
        /// @param start 起始时间点
        /// @return 时间间隔（Duration）
        inline Duration elapsedSince(TimePoint start)
        {
            return now() - start;
        }

        /// @brief 计算从 start 到现在经过的秒数（浮点精度）
        /// @param start 起始时间点
        /// @return 秒数，double 精度
        inline double elapsedSeconds(TimePoint start)
        {
            return std::chrono::duration<double>(elapsedSince(start)).count();
        }

        /// @brief 计算从 start 到现在经过的毫秒数
        /// @param start 起始时间点
        /// @return 毫秒数（int64_t）
        inline int64_t elapsedMilliseconds(TimePoint start)
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(elapsedSince(start)).count();
        }

        /// @brief 计算从 start 到现在经过的微秒数
        /// @param start 起始时间点
        /// @return 微秒数（int64_t）
        inline int64_t elapsedMicroseconds(TimePoint start)
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(elapsedSince(start)).count();
        }
    }  // namespace Clock
}  // namespace Incubator
