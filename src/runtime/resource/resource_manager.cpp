#include "runtime/resource/resource_manager.h"

#include <cstdint>
#include <exception>
#include <utility>

#include "runtime/core/log/logger.h"
#include "runtime/resource/resource_registry.h"
#include "runtime/resource/resource_state.h"

namespace Incubator
{
    namespace Resource
    {
        ResourceManager& ResourceManager::instance()
        {
            static ResourceManager s_instance;
            return s_instance;
        }

        void ResourceManager::registerLoader(ResourceType type, std::unique_ptr<LoaderBase> loader)
        {
            if (!loader)
            {
                return;
            }
            loaders_[type] = std::move(loader);
        }

        bool ResourceManager::hasLoader(ResourceType type) const
        {
            return loaders_.find(type) != loaders_.end();
        }

        bool ResourceManager::loadersEmpty() const
        {
            return loaders_.empty();
        }

        void ResourceManager::reset()
        {
            loaders_.clear();
        }

        ResourceId ResourceManager::registerResource(ResourceType type, std::string_view virtualPath)
        {
            return ResourceRegistry::instance().registerResource(type, virtualPath);
        }

        ResourceObject* ResourceManager::load(ResourceId id)
        {
            auto& reg = ResourceRegistry::instance();

            // 已加载 → 直接返回
            if (auto* obj = reg.getObject(id))
            {
                return obj;
            }

            // 未注册 → 日志 + nullptr
            ResourceType type = ResourceType::INVALID;
            std::string virtualPath;
            if (!reg.lookup(id, type, virtualPath))
            {
                Log::Registry::get("resource").error("load: unknown resource id");
                return nullptr;
            }

            // 无对应 Loader → 日志 + nullptr
            auto it = loaders_.find(type);
            if (it == loaders_.end())
            {
                Log::Registry::get("resource").error("load: no loader for type {:#x}",
                                                     static_cast<uint64_t>(type));
                return nullptr;
            }

            // 构建对象并挂载
            // 异常边界：Loader 的 IO 错误（权限/设备等）是可恢复事件，统一在此转换为 nullptr，
            // 保证 load() 不向外抛异常（"预期失败走返回值"由门面唯一实施）
            std::unique_ptr<ResourceObject> object;
            try
            {
                object = it->second->load(id, virtualPath);
            }
            catch (const std::exception& ex)
            {
                Log::Registry::get("resource").error("load: loader threw: {}", ex.what());
                return nullptr;
            }
            catch (...)
            {
                Log::Registry::get("resource").error("load: loader threw unknown exception");
                return nullptr;
            }
            if (!object)
            {
                return nullptr;  // Loader 内部已记录失败
            }
            object->setState(ResourceState::READY);
            if (!reg.attachObject(id, std::move(object)))
                return nullptr;
            Log::Registry::get("resource").debug("Loaded CPU resource '{}' (type {:#x})", virtualPath,
                                                  static_cast<uint64_t>(type));
            return reg.getObject(id);
        }

        void ResourceManager::acquire(ResourceId id)
        {
            ResourceRegistry::instance().acquire(id);
        }

        void ResourceManager::release(ResourceId id)
        {
            auto& reg = ResourceRegistry::instance();
            reg.release(id);
            if (reg.refCount(id) == 0)
            {
                reg.detachObject(id);  // 引用归零 → 卸载对象，登记保留
            }
        }
    }  // namespace Resource
}  // namespace Incubator
