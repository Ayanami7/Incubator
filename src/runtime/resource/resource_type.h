#pragma once

#include <cstdint>

#include "runtime/core/utils/hash.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief 资源类型标识（字符串哈希 tag）
        using ResourceTypeId = Utils::Hash;

        /// @brief 内置资源类型
        ///
        /// 显式值为字符串 constHash，跨会话稳定；INVALID = 0 作为哨兵（无效类型）。
        /// 枚举值使用 UPPER_CASE，与 Log::Level 约定一致。
        enum class BuiltinType : ResourceTypeId
        {
            TEXTURE = Utils::constHash("TEXTURE"),
            MODEL = Utils::constHash("MODEL"),
            MATERIAL = Utils::constHash("MATERIAL"),
            SHADER = Utils::constHash("SHADER"),
            INVALID = 0
        };
    }
}