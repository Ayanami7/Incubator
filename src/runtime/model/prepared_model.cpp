#include "runtime/model/prepared_model.h"

#include <limits>
#include <map>
#include <utility>

#include "runtime/core/error/error.h"
#include "runtime/render/renderer.h"
#include "runtime/resource/loader/texture_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"

namespace Incubator::Model
{
    namespace
    {
        constexpr uint32_t NO_INDEX = std::numeric_limits<uint32_t>::max();

        // Registration owns the first reference; an existing entry needs an additional hold,
        // even if a previous caller already released it to zero. Never change Loader routes.
        class CpuHold
        {
        public:
            CpuHold(Resource::ResourceType type, std::string_view path)
            {
                auto& manager = Resource::ResourceManager::instance();
                id_ = Resource::ResourceRegistry::instance().lookup(type, path);
                if (id_.valid())
                    manager.acquire(id_);
                else
                    id_ = manager.registerResource(type, path);
                if (!id_.valid())
                    throw Error::Exception(Error::Code::RENDER, fmt::format("Cannot register model asset: {}", path));
            }
            ~CpuHold()
            {
                if (id_.valid())
                    Resource::ResourceManager::instance().release(id_);
            }
            CpuHold(const CpuHold&) = delete;
            CpuHold& operator=(const CpuHold&) = delete;
            CpuHold(CpuHold&& other) noexcept : id_(std::exchange(other.id_, {})) {}

            template <typename T>
            T* get() const
            {
                return Resource::ResourceManager::instance().get<T>(id_);
            }

        private:
            Resource::ResourceId id_;
        };
    }  // namespace

    std::shared_ptr<const PreparedModel> prepareModel(Render::Renderer& renderer, std::string_view virtualPath)
    {
        CpuHold modelHold(Resource::ResourceType::MODEL, virtualPath);
        const auto* source = modelHold.get<Resource::ModelResource>();
        if (!source)
            throw Error::Exception(Error::Code::RENDER,
                                   fmt::format("Cannot prepare model '{}': model load failed", virtualPath));

        auto result = std::shared_ptr<PreparedModel>(new PreparedModel);
        result->nodes_ = source->nodes();
        result->rootNode_ = source->rootNode();
        std::vector<uint32_t> geometryMap(source->geometries().size(), NO_INDEX);
        std::vector<uint32_t> materialMap(source->materials().size(), NO_INDEX);
        std::vector<uint32_t> partMap(source->parts().size(), NO_INDEX);
        std::vector<uint32_t> geometries;
        std::vector<uint32_t> materials;
        for (auto& node : result->nodes_)
        {
            for (auto& partIndex : node.parts)
            {
                auto& mappedPart = partMap[partIndex];
                if (mappedPart == NO_INDEX)
                {
                    const auto& part = source->parts()[partIndex];
                    auto& geometry = geometryMap[part.geometry];
                    if (geometry == NO_INDEX)
                    {
                        geometry = static_cast<uint32_t>(geometries.size());
                        geometries.push_back(part.geometry);
                    }
                    auto& material = materialMap[part.material];
                    if (material == NO_INDEX)
                    {
                        material = static_cast<uint32_t>(materials.size());
                        materials.push_back(part.material);
                    }
                    mappedPart = static_cast<uint32_t>(result->parts_.size());
                    result->parts_.push_back({geometry, material});
                }
                partIndex = mappedPart;
            }
        }

        // Resolve every used dependency before creating GPU resources. Source data is already
        // validated by ModelResource; copied nodes/parts use dense result-local indices.
        std::vector<CpuHold> textureHolds;
        std::vector<const Resource::TextureResource*> pixels;
        std::map<std::string, uint32_t> textureMap;
        std::vector<uint32_t> materialTextures;
        uint32_t whiteIndex = NO_INDEX;
        for (uint32_t index : materials)
        {
            const auto& material = source->materials()[index];
            uint32_t textureIndex;
            if (material.texturePath)
            {
                auto [entry, inserted] =
                    textureMap.emplace(*material.texturePath, static_cast<uint32_t>(pixels.size()));
                textureIndex = entry->second;
                if (inserted)
                {
                    textureHolds.emplace_back(Resource::ResourceType::TEXTURE, *material.texturePath);
                    const auto* texture = textureHolds.back().get<Resource::TextureResource>();
                    if (!texture)
                        throw Error::Exception(Error::Code::RENDER,
                                               fmt::format("Cannot prepare model '{}': texture '{}' failed",
                                                           virtualPath, *material.texturePath));
                    pixels.push_back(texture);
                }
            }
            else
            {
                if (whiteIndex == NO_INDEX)
                {
                    whiteIndex = static_cast<uint32_t>(pixels.size());
                    pixels.push_back(nullptr);
                }
                textureIndex = whiteIndex;
            }
            materialTextures.push_back(textureIndex);
        }

        for (const auto* texture : pixels)
        {
            if (texture)
                result->textures_.push_back(renderer.createTexture(*texture));
            else
            {
                Resource::TextureResource white({}, 1, 1, {255, 255, 255, 255});
                result->textures_.push_back(renderer.createTexture(white));
            }
        }
        for (uint32_t index : geometries)
        {
            const auto& geometry = source->geometries()[index];
            std::vector<Render::MeshVertex> vertices;
            vertices.reserve(geometry.vertices.size());
            for (const auto& vertex : geometry.vertices)
                vertices.push_back(
                    {{vertex.position[0], vertex.position[1], vertex.position[2]}, {vertex.uv[0], vertex.uv[1]}});
            result->meshes_.push_back(renderer.createMesh(vertices, geometry.indices));
        }
        for (size_t i = 0; i < materials.size(); ++i)
            result->materials_.push_back(renderer.createMaterial(
                {result->textures_[materialTextures[i]], source->materials()[materials[i]].color}));

        // Cache model-relative draws once. The validated tree needs no per-frame traversal or
        // structure checks. Reverse the push order so children retain their source ordering.
        struct PendingNode
        {
            uint32_t index;
            glm::mat4 parent;
        };
        std::vector<PendingNode> pending{{result->rootNode_, glm::mat4(1.0f)}};
        bool boundsAvailable = true;
        while (!pending.empty())
        {
            const auto current = pending.back();
            pending.pop_back();
            const auto& node = result->nodes_[current.index];
            const auto world = current.parent * node.localTransform;
            for (uint32_t index : node.parts)
            {
                const auto& part = result->parts_[index];
                result->draws_.push_back({result->meshes_[part.geometry], result->materials_[part.material], world});
                if (boundsAvailable)
                {
                    const auto bounds = source->geometryBounds()[geometries[part.geometry]].transformed(world);
                    if (!bounds)
                        boundsAvailable = false;
                    else if (result->bounds_)
                        result->bounds_->merge(*bounds);
                    else
                        result->bounds_ = bounds;
                }
            }
            for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
                pending.push_back({*child, world});
        }
        if (!boundsAvailable)
            result->bounds_.reset();
        return result;
    }
}  // namespace Incubator::Model
