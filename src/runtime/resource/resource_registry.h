#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "runtime/core/utils/hash.h"
#include "runtime/resource/resource_id.h"
#include "runtime/resource/resource_type.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief 全局资源注册表
        ///
        /// 负责资源去重、哈希碰撞检测与引用计数。全局单例，线程安全由调用方保证。
        class ResourceRegistry
        {
        public:
            /// @brief 全局单例
            static ResourceRegistry& instance();

            /// @brief 清空全部注册条目（测试隔离 / 引擎关闭）
            void clear();

            /// @brief 注册资源
            /// @param type 资源类型
            /// @param virtualPath 虚拟路径（相对资源根）
            /// @return 既有或新建的资源 ID；非法输入 / 碰撞返回无效 ID
            ResourceId registerResource(ResourceType type, std::string_view virtualPath);

            /// @brief 直接注入条目（低层接口）
            ///
            /// 用途：① 反序列化/会话恢复时重建已持久化的 (type, key, id) 条目；
            ///       ② 测试构造哈希碰撞。key 必须是已规范化的虚拟路径。
            /// 双向校验：ID 已被不同 (type, key) 占用（碰撞），或 (type, key) 已被不同 ID 占用，均拒绝。
            /// @param type 资源类型
            /// @param key 已规范化（canonical）的虚拟路径
            /// @param id 目标资源 ID
            /// @return 注入成功 true；冲突返回 false
            bool injectEntry(ResourceType type, const std::string& key, ResourceId id);

            /// @brief 增加引用计数（未注册 ID 记录错误日志）
            void acquire(ResourceId id);

            /// @brief 减少引用计数（不会减到负数）
            void release(ResourceId id);

            /// @brief 查询引用计数
            /// @return 未注册 ID 返回 0
            uint32_t refCount(ResourceId id) const;

            /// @brief 按 ID 查询资源信息
            /// @param id 资源 ID
            /// @param type 输出资源类型
            /// @param virtualPath 输出规范化虚拟路径
            /// @return 未注册 ID 返回 false
            bool lookup(ResourceId id, ResourceType& type, std::string& virtualPath) const;

            /// @brief 按 (类型, 虚拟路径) 查询 ID
            /// @return 未注册返回无效 ID
            ResourceId lookup(ResourceType type, std::string_view virtualPath) const;

        private:
            struct Entry
            {
                ResourceType type = ResourceType::INVALID;
                std::string virtualPath;
                uint32_t refCount = 0;
            };

            ResourceRegistry() = default;

            // key = (类型, 规范化虚拟路径) → id
            std::map<std::pair<ResourceType, std::string>, ResourceId> pathToId_;
            // id → 条目
            std::map<Utils::Hash, Entry> idToEntry_;
        };
    }
}