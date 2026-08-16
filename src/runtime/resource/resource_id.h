#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "runtime/core/utils/hash.h"
#include "runtime/resource/resource_type.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief 将虚拟路径规范化为稳定键
        ///
        /// 顺序：normalize → 拒绝空/盘符/UNC → 剥离单个前导 '/' → ASCII 小写折叠。
        /// @param virtualPath 虚拟路径（相对资源根）
        /// @return 规范化键；非法输入（空、含盘符、UNC）返回空串
        std::string canonicalizeResourceKey(std::string_view virtualPath);

        /// @brief 全局稳定资源 ID
        ///
        /// 包装一个 64 位哈希值，0 表示无效 ID。
        /// ID = combine(域种子, 类型tag, hash(规范化键))，会话间稳定。
        class ResourceId
        {
        public:
            ResourceId() = default;

            /// @brief 由类型 + 虚拟路径计算 ID
            /// @param type 资源类型 tag
            /// @param virtualPath 虚拟路径（相对资源根）
            /// @return 稳定 ID；非法输入返回无效 ID
            static ResourceId fromPath(ResourceTypeId type, std::string_view virtualPath);

            /// @brief 由内置类型 + 虚拟路径计算 ID（便捷重载）
            /// @param type 内置资源类型
            /// @param virtualPath 虚拟路径（相对资源根）
            /// @return 稳定 ID；非法输入返回无效 ID
            static ResourceId fromPath(BuiltinType type, std::string_view virtualPath);

            /// @brief 仅由虚拟路径计算 ID（不带类型 tag）
            /// @param virtualPath 虚拟路径（相对资源根）
            /// @return 稳定 ID；非法输入返回无效 ID
            static ResourceId fromPathOnly(std::string_view virtualPath);

            /// @brief 是否为有效 ID
            bool valid() const noexcept;

            /// @brief 底层哈希值
            Utils::Hash value() const noexcept;

            explicit operator Utils::Hash() const noexcept;

            /// @brief 十六进制字符串表示
            std::string toString() const;

            /// @brief 从十六进制字符串反序列化
            /// @return 解析失败或值为 0 返回无效 ID
            static ResourceId fromString(std::string_view hex);

        private:
            explicit ResourceId(Utils::Hash value) noexcept;
            static ResourceId computeId(ResourceTypeId type, const std::string& key);

            Utils::Hash m_value = 0;
        };

        /// @brief 相等比较
        bool operator==(ResourceId lhs, ResourceId rhs) noexcept;

        /// @brief 小于比较（可作 map/set key）
        bool operator<(ResourceId lhs, ResourceId rhs) noexcept;
    }
}