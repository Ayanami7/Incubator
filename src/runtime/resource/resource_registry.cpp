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
                Log::Registry::get("resource").error("registerResource: invalid virtual path '{}'",
                                                     virtualPath);
                return {};
            }

            auto pathIt = m_pathToId.find(std::make_pair(type, key));
            if (pathIt != m_pathToId.end())
            {
                return pathIt->second;
            }

            ResourceId id = ResourceId::fromPath(type, key);
            return injectEntry(type, key, id) ? id : ResourceId{};
        }

        bool ResourceRegistry::injectEntry(ResourceType type, const std::string& key, ResourceId id)
        {
            if (!id.valid())
            {
                return false;
            }

            // (type, key) 已映射到不同 ID → 拒绝（封堵绕过 registerResource 导致的映射不一致）
            auto pathIt = m_pathToId.find(std::make_pair(type, key));
            if (pathIt != m_pathToId.end())
            {
                return pathIt->second == id;
            }

            auto [it, inserted] = m_idToEntry.emplace(id.value(), Entry{type, key, 1});
            if (!inserted)
            {
                // 碰撞检测：相同 ID 已被不同 (type, key) 占用 → 拒绝注入
                if (it->second.type != type || it->second.virtualPath != key)
                {
                    Log::Registry::get("resource").error(
                        "injectEntry: hash collision for '{}' (type {:#x}) with existing '{}'",
                        key, static_cast<uint64_t>(type), it->second.virtualPath);
                    return false;
                }
                return true;
            }

            m_pathToId.emplace(std::make_pair(type, key), id);
            return true;
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

        bool ResourceRegistry::lookup(ResourceId id, ResourceType& type, std::string& virtualPath) const
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

        ResourceId ResourceRegistry::lookup(ResourceType type, std::string_view virtualPath) const
        {
            std::string key = canonicalizeResourceKey(virtualPath);
            if (key.empty())
            {
                return {};
            }
            auto it = m_pathToId.find(std::make_pair(type, key));
            return it == m_pathToId.end() ? ResourceId{} : it->second;
        }
    }
}