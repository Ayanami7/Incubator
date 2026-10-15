#include "runtime/resource/resource_registry.h"

#include <utility>

#include "runtime/core/log/logger.h"

namespace Incubator
{
    namespace Resource
    {
        ResourceRegistry& ResourceRegistry::instance()
        {
            static ResourceRegistry s_instance;
            return s_instance;
        }

        void ResourceRegistry::clear()
        {
            pathToId_.clear();
            idToEntry_.clear();
        }

        ResourceId ResourceRegistry::registerResource(ResourceType type, std::string_view virtualPath)
        {
            if (type == ResourceType::INVALID)
            {
                Log::Registry::get("resource").error("registerResource: invalid resource type tag");
                return {};
            }

            std::string key = canonicalizeResourceKey(virtualPath);
            if (key.empty())
            {
                Log::Registry::get("resource").error("registerResource: invalid virtual path '{}'", virtualPath);
                return {};
            }

            auto pathIt = pathToId_.find(std::make_pair(type, key));
            if (pathIt != pathToId_.end())
            {
                return pathIt->second;
            }

            ResourceId id = ResourceId::fromPath(type, key);
            return injectEntry(type, key, id) ? id : ResourceId{};
        }

        bool ResourceRegistry::injectEntry(ResourceType type, const std::string& key, ResourceId id)
        {
            if (!id.valid() || type == ResourceType::INVALID || key.empty() ||
                key != canonicalizeResourceKey(key))
            {
                return false;
            }

            // (type, key) 已映射到不同 ID → 拒绝（封堵绕过 registerResource 导致的映射不一致）
            auto pathIt = pathToId_.find(std::make_pair(type, key));
            if (pathIt != pathToId_.end())
            {
                return pathIt->second == id;
            }

            auto [it, inserted] = idToEntry_.emplace(id.value(), Entry{{id, type, key, 0}, 1, nullptr});
            if (!inserted)
            {
                // 碰撞检测：相同 ID 已被不同 (type, key) 占用 → 拒绝注入
                if (it->second.info.type != type || it->second.info.virtualPath != key)
                {
                    Log::Registry::get("resource")
                        .error("injectEntry: hash collision for '{}' (type {:#x}) with existing '{}'", key,
                               static_cast<uint64_t>(type), it->second.info.virtualPath);
                    return false;
                }
                return true;
            }

            pathToId_.emplace(std::make_pair(type, key), id);
            return true;
        }

        void ResourceRegistry::acquire(ResourceId id)
        {
            auto it = idToEntry_.find(id.value());
            if (it == idToEntry_.end())
            {
                Log::Registry::get("resource").error("acquire: unknown resource id");
                return;
            }
            ++it->second.refCount;
        }

        void ResourceRegistry::release(ResourceId id)
        {
            auto it = idToEntry_.find(id.value());
            if (it == idToEntry_.end())
            {
                Log::Registry::get("resource").error("release: unknown resource id");
                return;
            }
            if (it->second.refCount > 0)
            {
                --it->second.refCount;
            }
            else
            {
                Log::Registry::get("resource").error("release: refCount already zero");
            }
        }

        uint32_t ResourceRegistry::refCount(ResourceId id) const
        {
            auto it = idToEntry_.find(id.value());
            return it == idToEntry_.end() ? 0 : it->second.refCount;
        }

        bool ResourceRegistry::lookup(ResourceId id, ResourceType& type, std::string& virtualPath) const
        {
            auto it = idToEntry_.find(id.value());
            if (it == idToEntry_.end())
            {
                return false;
            }
            type = it->second.info.type;
            virtualPath = it->second.info.virtualPath;
            return true;
        }

        ResourceId ResourceRegistry::lookup(ResourceType type, std::string_view virtualPath) const
        {
            std::string key = canonicalizeResourceKey(virtualPath);
            if (key.empty())
            {
                return {};
            }
            auto it = pathToId_.find(std::make_pair(type, key));
            return it == pathToId_.end() ? ResourceId{} : it->second;
        }

        bool ResourceRegistry::attachObject(ResourceId id, std::unique_ptr<ResourceObject> object)
        {
            auto it = idToEntry_.find(id.value());
            if (it == idToEntry_.end() || !object || it->second.object ||
                object->id() != it->second.info.id || object->type() != it->second.info.type)
            {
                Log::Registry::get("resource").error("attachObject: invalid object for id '{}'",
                                                     id.toString());
                return false;
            }
            it->second.object = std::move(object);
            return true;
        }

        const ResourceObject* ResourceRegistry::getObject(ResourceId id) const
        {
            auto it = idToEntry_.find(id.value());
            return (it == idToEntry_.end()) ? nullptr : it->second.object.get();
        }

        ResourceObject* ResourceRegistry::getObject(ResourceId id)
        {
            return const_cast<ResourceObject*>(std::as_const(*this).getObject(id));
        }

        void ResourceRegistry::detachObject(ResourceId id)
        {
            auto it = idToEntry_.find(id.value());
            if (it != idToEntry_.end() && it->second.object)
            {
                it->second.object.reset();
                Log::Registry::get("resource").debug("Unloaded CPU resource '{}' (type {:#x}, references {})",
                                                      it->second.info.virtualPath,
                                                      static_cast<uint64_t>(it->second.info.type),
                                                      it->second.refCount);
            }
        }
    }  // namespace Resource
}  // namespace Incubator
