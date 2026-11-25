#include "runtime/model/prepared_model.h"

#include <algorithm>
#include <limits>
#include <map>
#include <utility>
#include <variant>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/render/renderer.h"
#include "runtime/resource/loader/texture_resource.h"
#include "runtime/resource/resource_manager.h"

namespace Incubator::Model
{
    namespace
    {
        constexpr uint32_t NO_INDEX = std::numeric_limits<uint32_t>::max();
    }  // namespace

    std::shared_ptr<const PreparedModel> prepareModel(Render::Renderer& renderer, std::string_view virtualPath,
                                                    PrepareOptions options, ModelReadMode readMode)
    {
        ModelReference sourceReference(virtualPath, options);
        auto& resources = Resource::ResourceManager::instance();
        const auto requireUnloaded = [&](Resource::ResourceType type, std::string_view path) {
            const auto state = resources.query(type, path);
            if (state && (state->references != 0 || state->loaded))
                throw LoadError(fmt::format("Cannot reload model '{}': CPU resource '{}' is still held or loaded",
                                            virtualPath, path));
        };
        if (readMode == ModelReadMode::ReloadFromDisk)
            requireUnloaded(Resource::ResourceType::MODEL, sourceReference.path());
        auto modelHold = resources.hold<Resource::ModelResource>(Resource::ResourceType::MODEL, sourceReference.path());
        const auto* source = modelHold.get<Resource::ModelResource>();
        if (!source)
            throw LoadError(fmt::format("Cannot prepare model '{}': {}", virtualPath, modelHold.failure()->reason));

        auto result = std::shared_ptr<PreparedModel>(new PreparedModel(std::move(sourceReference)));
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

        // 创建 GPU 资源前先解析所有实际使用的依赖项。源数据已由
        // ModelResource 验证；复制后的节点和部件使用结果内连续索引。
        struct TextureUploadRequest
        {
            const Resource::TextureResource* resource = nullptr;
            const Resource::EmbeddedTexture* embedded = nullptr;
            Render::TextureColorSpace colorSpace = Render::TextureColorSpace::Srgb;
        };
        struct MaterialTextureSlots
        {
            uint32_t baseColor = NO_INDEX;
            uint32_t normal = NO_INDEX;
            uint32_t metallicRoughness = NO_INDEX;
            uint32_t emissive = NO_INDEX;
        };
        std::vector<Resource::CpuResource> textureHolds;
        std::vector<TextureUploadRequest> textureRequests;
        using TextureSource = std::variant<std::string, uint32_t>;
        std::map<std::pair<TextureSource, Render::TextureColorSpace>, uint32_t> textureMap;
        std::vector<MaterialTextureSlots> materialTextures;
        uint32_t whiteIndex = NO_INDEX;

        const auto usesPbrTextures = [&](const Resource::ModelMaterial& material) {
            return !material.unlit && options.shading == Render::ShadingMode::Pbr;
        };
        if (readMode == ModelReadMode::ReloadFromDisk)
        {
            // 先检查所有需要的贴图，避免同一贴图用于多个槽时把本次持有误判为外部持有。
            for (const auto index : materials)
            {
                const auto& material = source->materials()[index];
                if (material.texturePath)
                    requireUnloaded(Resource::ResourceType::TEXTURE, *material.texturePath);
                if (material.emissiveTexturePath)
                    requireUnloaded(Resource::ResourceType::TEXTURE, *material.emissiveTexturePath);
                if (usesPbrTextures(material))
                {
                    if (material.normalTexturePath)
                        requireUnloaded(Resource::ResourceType::TEXTURE, *material.normalTexturePath);
                    if (material.metallicRoughnessTexturePath)
                        requireUnloaded(Resource::ResourceType::TEXTURE, *material.metallicRoughnessTexturePath);
                }
            }
        }

        const auto acquireTexture = [&](const std::optional<std::string>& path, std::optional<uint32_t> embedded,
                                        Render::TextureColorSpace colorSpace) -> uint32_t {
            if (!path && !embedded)
                return NO_INDEX;
            TextureSource key = embedded ? TextureSource{*embedded} : TextureSource{*path};
            auto [entry, inserted] = textureMap.emplace(std::make_pair(std::move(key), colorSpace),
                                                        static_cast<uint32_t>(textureRequests.size()));
            if (!inserted)
                return entry->second;
            if (embedded)
            {
                textureRequests.push_back({nullptr, &source->embeddedTextures()[*embedded], colorSpace});
            }
            else
            {
                textureHolds.push_back(
                    resources.hold<Resource::TextureResource>(Resource::ResourceType::TEXTURE, *path));
                const auto* texture = textureHolds.back().get<Resource::TextureResource>();
                if (!texture)
                    throw LoadError(fmt::format("Cannot prepare model '{}': texture '{}': {}", virtualPath, *path,
                                                textureHolds.back().failure()->reason));
                textureRequests.push_back({texture, nullptr, colorSpace});
            }
            return entry->second;
        };

        for (uint32_t index : materials)
        {
            const auto& material = source->materials()[index];
            MaterialTextureSlots slots;
            slots.baseColor = acquireTexture(material.texturePath, material.embeddedTextureIndex,
                                             Render::TextureColorSpace::Srgb);
            if (slots.baseColor == NO_INDEX)
            {
                if (whiteIndex == NO_INDEX)
                {
                    whiteIndex = static_cast<uint32_t>(textureRequests.size());
                    textureRequests.push_back({});
                }
                slots.baseColor = whiteIndex;
            }
            if (usesPbrTextures(material))
            {
                slots.normal = acquireTexture(material.normalTexturePath, material.embeddedNormalTextureIndex,
                                              Render::TextureColorSpace::Linear);
                slots.metallicRoughness = acquireTexture(material.metallicRoughnessTexturePath,
                                                         material.embeddedMetallicRoughnessTextureIndex,
                                                         Render::TextureColorSpace::Linear);
            }
            slots.emissive = acquireTexture(material.emissiveTexturePath, material.embeddedEmissiveTextureIndex,
                                             Render::TextureColorSpace::Srgb);
            materialTextures.push_back(slots);
        }

        result->resources_ = renderer.createResources();
        for (const auto& request : textureRequests)
        {
            if (request.resource)
                result->textures_.push_back(
                    renderer.createTexture(result->resources_, *request.resource, options.mipMode, request.colorSpace));
            else if (request.embedded)
            {
                const auto& embedded = *request.embedded;
                const Resource::TextureResource texture({}, embedded.width, embedded.height, embedded.rgba8);
                result->textures_.push_back(
                    renderer.createTexture(result->resources_, texture, options.mipMode, request.colorSpace));
            }
            else
            {
                Resource::TextureResource white({}, 1, 1, {255, 255, 255, 255});
                result->textures_.push_back(
                    renderer.createTexture(result->resources_, white, options.mipMode, request.colorSpace));
            }
        }
        for (uint32_t index : geometries)
        {
            const auto& geometry = source->geometries()[index];
            std::vector<Render::MeshVertex> vertices;
            vertices.reserve(geometry.vertices.size());
            for (const auto& vertex : geometry.vertices)
                vertices.push_back({{vertex.position[0], vertex.position[1], vertex.position[2]},
                                    {vertex.uv[0], vertex.uv[1]},
                                    {vertex.normal[0], vertex.normal[1], vertex.normal[2]},
                                    {vertex.tangent[0], vertex.tangent[1], vertex.tangent[2], vertex.tangent[3]}});
            result->meshes_.push_back(renderer.createMesh(result->resources_, vertices, geometry.indices));
        }
        for (size_t i = 0; i < materials.size(); ++i)
        {
            const auto& sourceMat = source->materials()[materials[i]];
            const auto& slots = materialTextures[i];
            Render::MaterialDesc desc{
                result->textures_[slots.baseColor],
                sourceMat.color,
                options.sampler,
                sourceMat.unlit ? Render::ShadingMode::Unlit : options.shading,
                slots.normal == NO_INDEX ? Render::Texture{} : result->textures_[slots.normal],
                slots.metallicRoughness == NO_INDEX ? Render::Texture{} : result->textures_[slots.metallicRoughness],
                sourceMat.normalScale,
                sourceMat.metallic,
                sourceMat.roughness,
            };
            switch (sourceMat.alphaMode)
            {
                case Resource::AlphaMode::Opaque:
                    desc.alphaMode = Render::AlphaMode::Opaque;
                    break;
                case Resource::AlphaMode::Mask:
                    desc.alphaMode = Render::AlphaMode::Mask;
                    break;
                case Resource::AlphaMode::Blend:
                    desc.alphaMode = Render::AlphaMode::Blend;
                    break;
            }
            desc.alphaCutoff = sourceMat.alphaCutoff;
            desc.baseAlpha = sourceMat.baseAlpha;
            desc.doubleSided = sourceMat.doubleSided;
            desc.emissiveFactor = sourceMat.emissiveFactor;
            if (slots.emissive != NO_INDEX)
                desc.emissiveTexture = result->textures_[slots.emissive];
            result->materials_.push_back(renderer.createMaterial(result->resources_, desc));
            result->materialSlots_.push_back({materials[i], std::move(desc)});
        }

        // 模型相对绘制项只缓存一次。树结构已经验证，每帧无需重新遍历或
        // 检查结构。按相反顺序压入，以保持子节点的源顺序。
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
                result->drawMaterialSlots_.push_back(part.material);
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
        Log::Registry::get("render").debug(
            "Prepared model '{}': {} meshes, {} materials, {} textures; requested mip={}, min={}, mag={}, "
            "mipFilter={}, shading={}",
            result->source().path(), result->meshes_.size(), result->materials_.size(), result->textures_.size(),
            options.mipMode == Render::TextureMipMode::FullChain ? "fullChain" : "baseLevel",
            options.sampler.minFilter == Render::TextureFilter::Linear ? "linear" : "nearest",
            options.sampler.magFilter == Render::TextureFilter::Linear ? "linear" : "nearest",
            options.sampler.mipFilter == Render::MipFilter::BaseLevel ? "baseLevel" :
                options.sampler.mipFilter == Render::MipFilter::Nearest ? "nearest" : "linear",
            options.shading == Render::ShadingMode::Pbr     ? "pbr"
            : options.shading == Render::ShadingMode::Lambert ? "lambert"
                                                              : "unlit");
        return result;
    }

    const PreparedMaterialSlot* PreparedModel::findMaterialSlot(uint32_t sourceMaterialIndex) const noexcept
    {
        const auto found = std::ranges::find(materialSlots_, sourceMaterialIndex, &PreparedMaterialSlot::sourceMaterialIndex);
        return found == materialSlots_.end() ? nullptr : &*found;
    }

    std::shared_ptr<const PreparedMaterialOverride> prepareMaterialOverride(
        Render::Renderer& renderer, const PreparedModel& model, uint32_t sourceMaterialIndex,
        const std::array<float, 3>& color, Render::MaterialParameterMode roughnessMode, float roughness,
        Render::MaterialParameterMode metallicMode, float metallic)
    {
        const auto* slot = model.findMaterialSlot(sourceMaterialIndex);
        if (!slot)
            throw Error::Exception(Error::Code::RENDER,
                                   fmt::format("Material slot {} not found in model '{}'", sourceMaterialIndex,
                                               model.source().path()));
        if (slot->sourceDesc.shading != Render::ShadingMode::Pbr)
            throw Error::Exception(Error::Code::RENDER,
                                   fmt::format("Material slot {} in model '{}' is not PBR", sourceMaterialIndex,
                                               model.source().path()));
        Render::MaterialDesc desc = slot->sourceDesc;
        desc.color = color;
        desc.roughnessMode = roughnessMode;
        desc.roughness = roughness;
        desc.metallicMode = metallicMode;
        desc.metallic = metallic;
        auto resources = renderer.createResources();
        auto material = renderer.createMaterial(resources, desc);
        return std::make_shared<PreparedMaterialOverride>(PreparedMaterialOverride{std::move(resources), std::move(material)});
    }
}  // namespace Incubator::Model
