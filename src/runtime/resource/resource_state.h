#pragma once

#include <cstdint>

namespace Incubator
{
    namespace Resource
    {
        /// @brief 资源加载状态。
        ///
        /// 同步加载下 LOADING 为瞬时状态；同步失败直接返回 nullptr，
        /// 不会为失败结果挂载 ERROR 状态的对象。LOADING 与 ERROR 为异步加载预留。
        enum class ResourceState : uint8_t
        {
            UNLOADED = 0,  ///< 条目已登记但尚未挂载运行时对象。
            LOADING,       ///< 正在加载（为异步加载预留）。
            READY,         ///< 已加载，运行时对象可访问。
            ERROR          ///< 加载失败（为异步加载预留）。
        };
    }  // namespace Resource
}  // namespace Incubator
