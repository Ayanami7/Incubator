#include "runtime/resource/resource_manager.h"

#include <cstdint>
#include <exception>
#include <utility>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/resource/asset_source.h"
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

        void ResourceManager::bindAssetSource(AssetSource* source)
        {
            if (source == source_)
                return;
            if (!loaders_.empty() || ResourceRegistry::instance().hasLiveResources())
                throw Error::Exception(Error::Code::CONFIG,
                                       "Release CPU resources and clear Loader routes before changing asset source");
            source_ = source;
            if (source)
                Log::Registry::get("resource").debug("Bound asset root '{}'", Utils::Utf8::toUtf8String(source->root()));
            else
                Log::Registry::get("resource").debug("Unbound asset root");
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

        CpuResource::~CpuResource()
        {
            reset();
        }
        CpuResource::CpuResource(CpuResource&& other) noexcept
            : id_(std::exchange(other.id_, {})),
              object_(std::exchange(other.object_, nullptr)),
              failure_(std::move(other.failure_))
        {}
        CpuResource& CpuResource::operator=(CpuResource&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                id_ = std::exchange(other.id_, {});
                object_ = std::exchange(other.object_, nullptr);
                failure_ = std::move(other.failure_);
            }
            return *this;
        }
        void CpuResource::reset() noexcept
        {
            if (id_.valid())
                ResourceManager::instance().release(id_);
            id_ = {};
            object_ = nullptr;
        }

        std::vector<AssetEntry> ResourceManager::listDirectory(std::string_view directory) const
        {
            if (!source_)
                throw Error::Exception(Error::Code::CONFIG, "Bind an asset source before listing directories");
            return source_->listDirectory(directory);
        }

        std::optional<ResourceInfo> ResourceManager::query(ResourceType type, std::string_view path) const
        {
            const auto& registry = ResourceRegistry::instance();
            const auto id = registry.lookup(type, path);
            if (!id.valid())
                return std::nullopt;
            std::string key;
            registry.lookup(id, type, key);
            return ResourceInfo{id, type, std::move(key), registry.refCount(id), registry.getObject(id) != nullptr};
        }

        void ResourceManager::reportFailure(const ResourceFailure& failure)
        {
            Log::Registry::get("resource").error("load: '{}': {}", failure.path, failure.reason);
        }

        ResourceObject* ResourceManager::loadResource(ResourceId id, std::optional<ResourceFailure>& failure)
        {
            auto& reg = ResourceRegistry::instance();
            if (auto* object = reg.getObject(id))
                return object;
            ResourceType type = ResourceType::INVALID;
            std::string path;
            const auto fail = [&](std::string reason, bool reported = false, bool threw = false) -> ResourceObject* {
                failure =
                    ResourceFailure{path, fmt::format("{} (type {:#x}, id {:#x})", reason, uint64_t(type), id.value())};
                if (!reported)
                {
                    if (threw)
                        Log::Registry::get("resource").error("load: loader threw: '{}': {}", path, failure->reason);
                    else
                        reportFailure(*failure);
                }
                return nullptr;
            };
            if (!reg.lookup(id, type, path))
                return fail("unknown resource id " + id.toString());
            const auto route = loaders_.find(type);
            if (route == loaders_.end())
                return fail("no loader for resource type");
            try
            {
                auto result = route->second->loadResult(id, path);
                if (!result.object)
                    return fail(std::move(result.failure), result.reported);
                result.object->setState(ResourceState::READY);
                if (!reg.attachObject(id, std::move(result.object)))
                    return fail("Cannot mount CPU resource", true);
                Log::Registry::get("resource").debug("Loaded CPU resource '{}' (type {:#x})", path, uint64_t(type));
                return reg.getObject(id);
            }
            catch (const std::exception& error)
            {
                return fail(error.what(), false, true);
            }
            catch (...)
            {
                return fail("loader threw unknown exception", false, true);
            }
        }

        ResourceObject* ResourceManager::load(ResourceId id)
        {
            std::optional<ResourceFailure> failure;
            return loadResource(id, failure);
        }

        CpuResource ResourceManager::holdResource(ResourceType type, std::string_view path)
        {
            CpuResource result;
            auto& reg = ResourceRegistry::instance();
            result.id_ = reg.lookup(type, path);
            if (result.id_.valid())
                acquire(result.id_);
            else
                result.id_ = registerResource(type, path);
            if (!result.id_.valid())
            {
                result.failure_ = ResourceFailure{std::string(path), "Cannot register CPU resource"};
                // 登记失败由 Registry 记录。
                return result;
            }
            result.object_ = loadResource(result.id_, result.failure_);
            if (!result.object_)
                result.reset();
            return result;
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
