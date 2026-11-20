#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "runtime/resource/asset_source.h"
#include "runtime/resource/loader/loader_base.h"
#include "runtime/resource/resource_id.h"
#include "runtime/resource/resource_type.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief 本次加载失败的自有值；后续加载不会覆盖路径和原因。
        struct ResourceFailure
        {
            std::string path;
            std::string reason;
        };
        /// @brief 按值返回登记状态；查询不加载或增加持有。
        struct ResourceInfo
        {
            ResourceId id;
            ResourceType type;
            std::string path;
            uint32_t references;
            bool loaded;
        };
        class ResourceManager;
        /// @brief 只可移动的单次 CPU 持有；指针仅在持有存活时借用，须在会话解绑前释放。
        class CpuResource
        {
        public:
            CpuResource() = default;
            ~CpuResource();
            CpuResource(const CpuResource&) = delete;
            CpuResource& operator=(const CpuResource&) = delete;
            CpuResource(CpuResource&& other) noexcept;
            CpuResource& operator=(CpuResource&& other) noexcept;
            /// @brief 释放本次持有；失败信息仍随返回值保留。
            void reset() noexcept;
            ResourceId id() const noexcept
            {
                return id_;
            }
            explicit operator bool() const noexcept
            {
                return object_ != nullptr;
            }
            /// @brief 借用已加载对象；空持有或类型不匹配时返回空指针，不触发加载。
            template <class T>
            T* get() const noexcept
            {
                return dynamic_cast<T*>(object_);
            }
            const std::optional<ResourceFailure>& failure() const noexcept
            {
                return failure_;
            }

        private:
            friend class ResourceManager;
            ResourceId id_;
            ResourceObject* object_ = nullptr;
            std::optional<ResourceFailure> failure_;
        };

        /// @brief 资源门面：Loader 路由 + 生命周期驱动
        ///
        /// 单例。存储层委托 ResourceRegistry；Loader 路由表在本类。
        /// 线程安全由调用方保证（与 Registry 约定一致）。
        ///
        /// ## 引用计数所有权模型
        /// - 消费者统一通过 `hold<T>(type, path)` 取得只可移动的 `CpuResource` 持有
        /// - 首次登记或重复持有增加引用计数，共享已挂载的 CPU 对象
        /// - `CpuResource` 析构或 `reset()` 时递减计数，归零时卸载对象（`detachObject`），登记保留
        /// - 通过 `CpuResource::get<T>()` 借用的裸指针**仅在对应持有存活期内有效**
        ///
        /// ## 异常边界
        /// `hold<T>()` 保证不向外抛异常：任何 Loader 抛出的异常视为该资源加载失败（日志 + `failure()`）。
        class ResourceManager
        {
        public:
            /// @brief 全局单例
            static ResourceManager& instance();

            /// @brief 绑定或解除单个借用资产源；空指针解除绑定，同一源重复绑定无操作。
            /// @note 换绑或解除前须清除 Loader，并释放全部 CPU 持有及挂载对象；失败保留原状态。
            /// 源须存活至解除绑定，reset() 仅清除路由，不解除源或销毁 CPU 对象。
            /// @throws Error::Exception 尚有路由、CPU 持有或挂载对象时抛 CONFIG。
            void bindAssetSource(AssetSource* source);
            /// @brief 返回当前借用源；尚未绑定时为空。
            AssetSource* assetSource() const noexcept
            {
                return source_;
            }

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

            /// @brief 列出当前资产源目录；不登记、不加载；未绑定或访问失败抛异常。
            std::vector<AssetEntry> listDirectory(std::string_view directory = {}) const;
            /// @brief 查询既有登记，未登记返回空值；不加载或改变计数。
            std::optional<ResourceInfo> query(ResourceType type, std::string_view path) const;
            /// @brief 取得恰好一次 CPU 持有；失败值保存路径和原因，且不保留引用。
            /// @note 失败只由此边界记录一次；复制禁止，移动转移持有，reset 或析构时释放。
            template <class T = ResourceObject>
            CpuResource hold(ResourceType type, std::string_view path)
            {
                auto result = holdResource(type, path);
                if (result && !result.get<T>())
                {
                    result.reset();
                    result.failure_ = ResourceFailure{std::string(path), "CPU resource type mismatch"};
                    reportFailure(*result.failure_);
                }
                return result;
            }

        private:
            friend class CpuResource;
            ResourceManager() = default;
            CpuResource holdResource(ResourceType type, std::string_view path);
            ResourceObject* loadResource(ResourceId id, std::optional<ResourceFailure>& failure);
            void release(ResourceId id);
            static void reportFailure(const ResourceFailure& failure);
            AssetSource* source_ = nullptr;
            std::map<ResourceType, std::unique_ptr<LoaderBase>> loaders_;
        };
    }  // namespace Resource
}  // namespace Incubator
