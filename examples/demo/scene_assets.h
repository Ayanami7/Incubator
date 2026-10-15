#pragma once

#include <array>
#include <memory>
#include <string_view>
#include <utility>

#include "runtime/core/error/error.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/resource/loader/shader_loader.h"
#include "runtime/resource/loader/shader_resource.h"
#include "runtime/resource/loader/texture_loader.h"
#include "runtime/resource/loader/texture_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"

namespace Incubator
{
    namespace Demo
    {
        struct SceneAssetPaths
        {
            std::string_view vertex = "shaders/cube.vert.spv";
            std::string_view fragment = "shaders/cube.frag.spv";
            std::string_view texture = "textures/reference_rgba.png";
        };

        // Demo and review tests exclusively own the Manager's loader routes during this scope.
        // The already-constructed cleanup member also covers failure of the outer constructor.
        class SceneAssets
        {
        private:
            struct Cleanup
            {
                ~Cleanup()
                {
                    finish();
                }

                void finish()
                {
                    if (!active)
                        return;
                    auto& manager = Resource::ResourceManager::instance();
                    for (size_t index = 0; index < count; ++index)
                        manager.release(ids[index]);
                    manager.reset();
                    active = false;
                }

                std::array<Resource::ResourceId, 3> ids{};
                size_t count = 0;
                bool active = true;
            };

            template <class T>
            T* load(Resource::ResourceType type, std::string_view path)
            {
                auto& manager = Resource::ResourceManager::instance();
                auto id = Resource::ResourceRegistry::instance().lookup(type, path);
                if (id.valid())
                    manager.acquire(id);
                else
                    id = manager.registerResource(type, path);
                if (!id.valid())
                    throw Error::Exception(Error::Code::RENDER, fmt::format("Cannot register scene asset: {}", path));
                cleanup_.ids[cleanup_.count++] = id;
                auto* object = manager.get<T>(id);
                if (!object)
                    throw Error::Exception(Error::Code::RENDER, fmt::format("Cannot load scene asset: {}", path));
                return object;
            }

            std::unique_ptr<FileIO::FileIO> fileIO_;
            Cleanup cleanup_;

        public:
            explicit SceneAssets(SceneAssetPaths paths = {},
                                 std::unique_ptr<FileIO::FileIO> fileIO = FileIO::createPlatformFileIO())
                : fileIO_(std::move(fileIO))
            {
                auto& manager = Resource::ResourceManager::instance();
                manager.registerLoader(Resource::ResourceType::SHADER,
                                       std::make_unique<Resource::ShaderLoader>(*fileIO_));
                manager.registerLoader(Resource::ResourceType::TEXTURE,
                                       std::make_unique<Resource::TextureLoader>(*fileIO_));
                vertex = load<Resource::ShaderResource>(Resource::ResourceType::SHADER, paths.vertex);
                fragment = load<Resource::ShaderResource>(Resource::ResourceType::SHADER, paths.fragment);
                texture = load<Resource::TextureResource>(Resource::ResourceType::TEXTURE, paths.texture);
            }

            SceneAssets(const SceneAssets&) = delete;
            SceneAssets& operator=(const SceneAssets&) = delete;

            void releaseCpuResources()
            {
                cleanup_.finish();
                vertex = nullptr;
                fragment = nullptr;
                texture = nullptr;
            }

            Resource::ShaderResource* vertex = nullptr;
            Resource::ShaderResource* fragment = nullptr;
            Resource::TextureResource* texture = nullptr;
        };
    }  // namespace Demo
}  // namespace Incubator
