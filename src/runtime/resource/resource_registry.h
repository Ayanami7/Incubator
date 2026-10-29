#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "runtime/core/utils/hash.h"
#include "runtime/resource/asset_info.h"
#include "runtime/resource/resource_id.h"
#include "runtime/resource/resource_object.h"
#include "runtime/resource/resource_type.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief 全局资源注册表
        ///
        /// 负责资源去重、哈希碰撞检测与引用计数，并作为运行时对象的唯一挂载点。
        /// 全局单例，线程安全由调用方保证。
        ///
        /// refCount 契约：注册即 refCount = 1；load() 不加引用；
        /// release() 降到 0 时由调用方（ResourceManager）卸载对象（detachObject），登记保留。
        /// Registry 自身允许 refCount == 0 且暂持有 object 的纯存储态，不做自动卸载。
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
            /// 输入校验：type 非 INVALID；key 非空且必须已规范化（canonical roundtrip）。
            /// 双向校验：ID 已被不同 (type, key) 占用（碰撞），或 (type, key) 已被不同 ID 占用，均拒绝。
            /// @param type 资源类型
            /// @param key 已规范化（canonical）的虚拟路径
            /// @param id 目标资源 ID
            /// @return 注入成功 true；非法输入 / 冲突返回 false
            bool injectEntry(ResourceType type, const std::string& key, ResourceId id);

            /// @brief 增加引用计数（未注册 ID 记录错误日志）
            void acquire(ResourceId id);

            /// @brief 减少引用计数（不会减到负数）
            void release(ResourceId id);

            /// @brief 是否仍有 CPU 持有或挂载对象；用于资产源换绑检查。
            bool hasLiveResources() const;

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

            /// @brief 挂载加载好的对象到条目
            /// @param id 资源 ID
            /// @param object 加载好的运行时对象；id/type 必须与登记信息一致
            /// @return 未注册 ID / object 为空 / 条目已挂载 / id 或 type 与登记信息不一致 返回 false
            bool attachObject(ResourceId id, std::unique_ptr<ResourceObject> object);

            /// @brief 获取条目的运行时对象
            /// @param id 资源 ID
            /// @return 未注册或未加载返回 nullptr
            ResourceObject* getObject(ResourceId id);
            const ResourceObject* getObject(ResourceId id) const;

            /// @brief 卸载对象（对象置空，登记保留）；未注册 ID 为 no-op
            void detachObject(ResourceId id);

        private:
            struct Entry
            {
                AssetInfo info;                          // 身份信息（id/type/path/contentHash）
                uint32_t refCount = 1;                   // 引用计数
                std::unique_ptr<ResourceObject> object;  // 运行时对象（null = 未加载）
            };

            ResourceRegistry() = default;

            // key = (类型, 规范化虚拟路径) → id
            std::map<std::pair<ResourceType, std::string>, ResourceId> pathToId_;
            // id → 条目
            std::map<Utils::Hash, Entry> idToEntry_;
        };
    }  // namespace Resource
}  // namespace Incubator