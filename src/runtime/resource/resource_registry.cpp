#include "runtime/resource/resource_registry.h"

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
            m_pathToId.clear();
            m_idToEntry.clear();
        }

        ResourceId ResourceRegistry::registerResource(ResourceTypeId type, std::string_view virtualPath)
        {
            if (type == static_cast<ResourceTypeId>(BuiltinType::INVALID))
            {
                Log::Registry::get("resource").error("registerResource: invalid resource type tag");
                return {};
            }

            std::string key = canonicalizeResourceKey(virtualPath);
            if (key.empty())
            {
                Log::Registry::get("resource").error("registerResource: invalid virtual path '{}'",
                                                     virtualPath);
                return {};
            }

            auto pathIt = m_pathToId.find(std::make_pair(type, key));
            if (pathIt != m_pathToId.end())
            {
                return pathIt->second;
            }

            return registerWithId(type, key, ResourceId::fromPath(type, key));
        }

        ResourceId ResourceRegistry::registerResource(BuiltinType type, std::string_view virtualPath)
        {
            return registerResource(static_cast<ResourceTypeId>(type), virtualPath);
        }

        ResourceId ResourceRegistry::registerWithId(ResourceTypeId type, const std::string& key,
                                                    ResourceId id)
        {
            if (!id.valid())
            {
                return {};
            }

            auto [it, inserted] = m_idToEntry.emplace(id.value(), Entry{type, key, 1});
            if (!inserted)
            {
                // 碰撞检测：相同 ID 已被不同 (type, key) 占用 → 拒绝注册
                if (it->second.type != type || it->second.virtualPath != key)
                {
                    Log::Registry::get("resource").error(
                        "registerResource: hash collision for '{}' (type {:#x}) with existing '{}'",
                        key, static_cast<uint64_t>(type), it->second.virtualPath);
                    return {};
                }
                // 相同 (type, key) 已存在（防御分支，正常流程已被 pathToId 命中拦截）
                return id;
            }

            m_pathToId.emplace(std::make_pair(type, key), id);
            return id;
        }

        ResourceId ResourceRegistry::registerWithId(BuiltinType type, const std::string& key,
                                                    ResourceId id)
        {
            return registerWithId(static_cast<ResourceTypeId>(type), key, id);
        }

        void ResourceRegistry::acquire(ResourceId id)
        {
            auto it = m_idToEntry.find(id.value());
            if (it == m_idToEntry.end())
            {
                Log::Registry::get("resource").error("acquire: unknown resource id");
                return;
            }
            ++it->second.refCount;
        }

        void ResourceRegistry::release(ResourceId id)
        {
            auto it = m_idToEntry.find(id.value());
            if (it == m_idToEntry.end())
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
            auto it = m_idToEntry.find(id.value());
            return it == m_idToEntry.end() ? 0 : it->second.refCount;
        }

        bool ResourceRegistry::lookup(ResourceId id, ResourceTypeId& type, std::string& virtualPath) const
        {
            auto it = m_idToEntry.find(id.value());
            if (it == m_idToEntry.end())
            {
                return false;
            }
            type = it->second.type;
            virtualPath = it->second.virtualPath;
            return true;
        }

        ResourceId ResourceRegistry::lookup(ResourceTypeId type, std::string_view virtualPath) const
        {
            std::string key = canonicalizeResourceKey(virtualPath);
            if (key.empty())
            {
                return {};
            }
            auto it = m_pathToId.find(std::make_pair(type, key));
            return it == m_pathToId.end() ? ResourceId{} : it->second;
        }

        ResourceId ResourceRegistry::lookup(BuiltinType type, std::string_view virtualPath) const
        {
            return lookup(static_cast<ResourceTypeId>(type), virtualPath);
        }
    }
}
