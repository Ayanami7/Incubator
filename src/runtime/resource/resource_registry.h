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
            /// @param type 资源类型 tag
            /// @param virtualPath 虚拟路径（相对资源根）
            /// @return 既有或新建的资源 ID；非法输入 / 碰撞返回无效 ID
            ResourceId registerResource(ResourceTypeId type, std::string_view virtualPath);

            /// @brief 由内置类型注册资源（便捷重载）
            /// @param type 内置资源类型
            /// @param virtualPath 虚拟路径（相对资源根）
            /// @return 既有或新建的资源 ID；非法输入 / 碰撞返回无效 ID
            ResourceId registerResource(BuiltinType type, std::string_view virtualPath);

            /// @brief 以指定 ID 登记（测试注入接口）
            /// @param type 资源类型 tag
            /// @param key 已规范化（canonical）的虚拟路径
            /// @param id 目标资源 ID
            /// @return 成功返回 id；ID 已被不同 (type, key) 占用（碰撞）返回无效 ID
            ResourceId registerWithId(ResourceTypeId type, const std::string& key, ResourceId id);

            /// @brief 以指定 ID 登记内置类型资源（便捷重载）
            ResourceId registerWithId(BuiltinType type, const std::string& key, ResourceId id);

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
            bool lookup(ResourceId id, ResourceTypeId& type, std::string& virtualPath) const;

            /// @brief 按 (类型, 虚拟路径) 查询 ID
            /// @return 未注册返回无效 ID
            ResourceId lookup(ResourceTypeId type, std::string_view virtualPath) const;

            /// @brief 按 (内置类型, 虚拟路径) 查询 ID（便捷重载）
            /// @return 未注册返回无效 ID
            ResourceId lookup(BuiltinType type, std::string_view virtualPath) const;

        private:
            struct Entry
            {
                ResourceTypeId type = static_cast<ResourceTypeId>(BuiltinType::INVALID);
                std::string virtualPath;
                uint32_t refCount = 0;
            };

            ResourceRegistry() = default;

            // key = (类型, 规范化虚拟路径) → id
            std::map<std::pair<ResourceTypeId, std::string>, ResourceId> m_pathToId;
            // id → 条目
            std::map<Utils::Hash, Entry> m_idToEntry;
        };
    }  // namespace Resource
}  // namespace Incubator
