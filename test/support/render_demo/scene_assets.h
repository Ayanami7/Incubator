#pragma once

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

namespace Incubator::Demo
{
    struct SceneAssetPaths
    {
        std::string_view vertex = "shaders/cube.vert.spv";
        std::string_view fragment = "shaders/cube.frag.spv";
        std::string_view displayVertex = "shaders/display.vert.spv";
        std::string_view displayFragment = "shaders/display.frag.spv";
        std::string_view texture = "textures/reference_rgba.png";
    };

    // 在此作用域内，渲染测试独占 Manager 的 Shader/Texture Loader 路由。
    class SceneAssets
    {
    public:
        explicit SceneAssets(SceneAssetPaths paths = {},
                             std::unique_ptr<FileIO::FileIO> fileIO = FileIO::createPlatformFileIO())
            : fileIO_(std::move(fileIO)), source_(*fileIO_)
        {
            auto& manager = Resource::ResourceManager::instance();
            manager.registerLoader(Resource::ResourceType::SHADER, std::make_unique<Resource::ShaderLoader>(source_));
            manager.registerLoader(Resource::ResourceType::TEXTURE, std::make_unique<Resource::TextureLoader>(source_));
            vertexHold_ = hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, paths.vertex);
            fragmentHold_ = hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, paths.fragment);
            displayVertexHold_ = hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, paths.displayVertex);
            displayFragmentHold_ =
                hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, paths.displayFragment);
            textureHold_ = hold<Resource::TextureResource>(Resource::ResourceType::TEXTURE, paths.texture);
            vertex = vertexHold_.get<Resource::ShaderResource>();
            fragment = fragmentHold_.get<Resource::ShaderResource>();
            displayVertex = displayVertexHold_.get<Resource::ShaderResource>();
            displayFragment = displayFragmentHold_.get<Resource::ShaderResource>();
            texture = textureHold_.get<Resource::TextureResource>();
        }

        SceneAssets(const SceneAssets&) = delete;
        SceneAssets& operator=(const SceneAssets&) = delete;

        void releaseCpuResources()
        {
            vertex = nullptr;
            fragment = nullptr;
            displayVertex = nullptr;
            displayFragment = nullptr;
            texture = nullptr;
            textureHold_.reset();
            displayFragmentHold_.reset();
            displayVertexHold_.reset();
            fragmentHold_.reset();
            vertexHold_.reset();
            routeCleanup_.finish();
        }

        Resource::ShaderResource* vertex = nullptr;
        Resource::ShaderResource* fragment = nullptr;
        Resource::ShaderResource* displayVertex = nullptr;
        Resource::ShaderResource* displayFragment = nullptr;
        Resource::TextureResource* texture = nullptr;

    private:
        struct RouteCleanup
        {
            ~RouteCleanup() { finish(); }

            void finish()
            {
                if (!active)
                    return;
                Resource::ResourceManager::instance().reset();
                active = false;
            }

            bool active = true;
        };

        template <class T>
        static Resource::CpuResource hold(Resource::ResourceType type, std::string_view path)
        {
            auto resource = Resource::ResourceManager::instance().hold<T>(type, path);
            if (!resource.template get<T>())
                throw Error::Exception(Error::Code::RENDER, fmt::format("Cannot load scene asset: {}", path));
            return resource;
        }

        std::unique_ptr<FileIO::FileIO> fileIO_;
        Resource::AssetSource source_;
        RouteCleanup routeCleanup_;
        Resource::CpuResource vertexHold_;
        Resource::CpuResource fragmentHold_;
        Resource::CpuResource displayVertexHold_;
        Resource::CpuResource displayFragmentHold_;
        Resource::CpuResource textureHold_;
    };
}  // namespace Incubator::Demo
