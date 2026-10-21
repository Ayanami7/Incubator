#pragma once

#include <map>
#include <memory>
#include <string_view>

#include "runtime/resource/loader/loader_base.h"
#include "runtime/resource/resource_id.h"
#include "runtime/resource/resource_type.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief 资源门面：Loader 路由 + 生命周期驱动
        ///
        /// 单例。存储层委托 ResourceRegistry；Loader 路由表在本类。
        /// 线程安全由调用方保证（与 Registry 约定一致）。
        ///
        /// ## 引用计数所有权模型
        /// - `registerResource` 首次登记即持有 refCount = 1；重复登记幂等，**不再 +1**
        /// - 额外的持有者必须显式调用 `acquire()`；`load()`/`get()` 不改变引用计数
        /// - `release()` 递减计数，归零时由本类卸载对象（`detachObject`），登记保留
        /// - `load()`/`get()` 返回的裸指针**仅在对应持有期（登记持有 + acquire）内有效**，
        ///   持有期满后不得再访问
        ///
        /// ## 异常边界
        /// `load()` 保证不向外抛异常：任何 Loader 抛出的异常视为该资源加载失败（日志 + nullptr）。
        class ResourceManager
        {
        public:
            /// @brief 全局单例
            static ResourceManager& instance();

            /// @brief 注册 Loader（按类型路由；同类型重复注册覆盖）
            /// @param type 资源类型
            /// @param loader 加载器（manager 接管所有权）
            void registerLoader(ResourceType type, std::unique_ptr<LoaderBase> loader);

            /// @brief 是否已注册该类型的 Loader
            bool hasLoader(ResourceType type) const;

            /// @brief 整个 Loader 路由表是否为空（不局限于已知或当前会话使用的类型）
            bool loadersEmpty() const;

            /// @brief 清除全部 Loader（测试隔离 / 引擎关闭）
            void reset();

            /// @brief 门面：注册资源（委托 Registry 幂等语义）
            /// @return 既有或新建的资源 ID；非法输入返回无效 ID
            ResourceId registerResource(ResourceType type, std::string_view virtualPath);

            /// @brief 同步加载
            ///
            /// 已挂载对象直接返回；否则按类型路由到 Loader 构建并挂载。
            /// @param id 资源 ID
            /// @return 成功返回 READY 对象；未注册 / 无 Loader / 加载失败 / Loader 抛异常 均返回 nullptr
            ResourceObject* load(ResourceId id);

            /// @brief 类型安全取回（load + dynamic_cast；类型不符返回 nullptr）
            template <typename T>
            T* get(ResourceId id);

            /// @brief 增加引用（额外持有者使用；未注册 ID 记录错误日志）
            void acquire(ResourceId id);

            /// @brief 释放引用：委托 Registry 计数；归零时卸载对象（登记保留）
            void release(ResourceId id);

        private:
            ResourceManager() = default;

            std::map<ResourceType, std::unique_ptr<LoaderBase>> loaders_;
        };

        template <typename T>
        T* ResourceManager::get(ResourceId id)
        {
            return dynamic_cast<T*>(load(id));
        }
    }  // namespace Resource
}  // namespace Incubator