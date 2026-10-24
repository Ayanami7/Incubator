#include "runtime/resource/loader/model_loader.h"

#include <assimp/GltfMaterial.h>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <assimp/DefaultLogger.hpp>
#include <assimp/Importer.hpp>
#include <assimp/LogStream.hpp>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/path/path.h"
#include "runtime/resource/loader/detail/assimp_fileio.h"
#include "runtime/resource/loader/detail/model_source.h"
#include "runtime/resource/loader/model_resource.h"

namespace Incubator::Resource
{
    namespace
    {
        // 固定配置的导入器会丢弃格式错误的面，并可能忽略格式错误的 UV 流。
        // 只监视这些诊断，避免将修复后但不完整的网格发布出去。
        class ImportDiagnostics final : public Assimp::LogStream
        {
        public:
            ImportDiagnostics() : ownsLogger_(Assimp::DefaultLogger::isNullLogger())
            {
                if (ownsLogger_)
                    Assimp::DefaultLogger::create("", Assimp::Logger::NORMAL, 0);
                Assimp::DefaultLogger::get()->attachStream(this, Assimp::Logger::Warn | Assimp::Logger::Err);
            }
            ~ImportDiagnostics() override
            {
                Assimp::DefaultLogger::get()->detachStream(this, Assimp::Logger::Warn | Assimp::Logger::Err);
                if (ownsLogger_)
                    Assimp::DefaultLogger::kill();
            }
            void write(const char* message) override
            {
                const std::string_view text(message);
                if (failure.empty() && (text.find("faces had out-of-range indices") != std::string_view::npos ||
                                        text.find("not compatible with the TRIANGLES mode") != std::string_view::npos ||
                                        text.find("Texcoord stream size") != std::string_view::npos ||
                                        text.find("Unsupported material requested") != std::string_view::npos ||
                                        text.find("failed to locate material") != std::string_view::npos))
                {
                    failure = message;
                }
            }
            std::string failure;

        private:
            bool ownsLogger_;
        };

        ModelMaterial convertMaterial(const aiMaterial& source, uint32_t index, bool gltf, std::string_view directory,
                                      bool objDiffuseSpecified)
        {
            const auto reject = [index](std::string_view reason) {
                throw Error::Exception(Error::Code::IO, fmt::format("Model material {}: {}", index, reason));
            };
            if (gltf)
            {
                aiString mode;
                if (source.Get(AI_MATKEY_GLTF_ALPHAMODE, mode) != aiReturn_SUCCESS ||
                    std::string_view(mode.C_Str()) != "OPAQUE")
                {
                    reject("Only glTF alphaMode OPAQUE is supported");
                }
                // 即使材质为 OPAQUE，Assimp 也会把 baseColorFactor.a 映射到 OPACITY。此处不要检查该值。
            }
            else
            {
                float opacity = 1.0f;
                source.Get(AI_MATKEY_OPACITY, opacity);
                if (!std::isfinite(opacity) || opacity != 1.0f || source.GetTextureCount(aiTextureType_OPACITY) != 0)
                {
                    reject("OBJ requires opacity=1 and no map_d");
                }
            }
            ModelMaterial result;
            if (gltf)
            {
                aiShadingMode shading = aiShadingMode_PBR_BRDF;
                source.Get(AI_MATKEY_SHADING_MODEL, shading);
                result.unlit = shading == aiShadingMode_Unlit;
            }
            aiColor4D color(1.0f, 1.0f, 1.0f, 1.0f);
            if (gltf)
                source.Get(AI_MATKEY_BASE_COLOR, color);
            else if (objDiffuseSpecified)
                source.Get(AI_MATKEY_COLOR_DIFFUSE, color);
            result.color = {color.r, color.g, color.b};
            for (float value : result.color)
            {
                if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
                {
                    reject("Requires finite linear RGB in [0, 1]");
                }
            }

            const aiTextureType textureType = gltf ? aiTextureType_BASE_COLOR : aiTextureType_DIFFUSE;
            const unsigned count = source.GetTextureCount(textureType);
            if (count > 1)
                reject("Multiple base color textures are unsupported");
            if (count == 1)
            {
                aiString path;
                aiTextureMapping mapping = aiTextureMapping_UV;
                unsigned uv = 0;
                if (source.GetTexture(textureType, 0, &path, &mapping, &uv) != aiReturn_SUCCESS ||
                    mapping != aiTextureMapping_UV || uv != 0)
                {
                    reject("Base color texture requires UV0");
                }
                aiUVTransform transform;
                if (source.Get(AI_MATKEY_UVTRANSFORM(textureType, 0), transform) == aiReturn_SUCCESS)
                {
                    reject("Base color UV transform is unsupported");
                }
                if (path.length == 0 || path.data[0] == '*')
                    reject("Embedded model image is unsupported");
                result.texturePath = Detail::modelDependencyPath(directory, path.C_Str(), gltf);
                if (!Path::hasExtension(*result.texturePath, "png"))
                    reject("Only external PNG images are supported");
            }
            return result;
        }

        glm::mat4 convertMatrix(const aiMatrix4x4& matrix)
        {
            // aiMatrix4x4 按行命名 a..d；GLM 构造函数接收列。
            return glm::mat4(matrix.a1, matrix.b1, matrix.c1, matrix.d1, matrix.a2, matrix.b2, matrix.c2, matrix.d2,
                             matrix.a3, matrix.b3, matrix.c3, matrix.d3, matrix.a4, matrix.b4, matrix.c4, matrix.d4);
        }
    }  // namespace

    ModelLoader::ModelLoader(FileIO::FileIO& fileIO) : fileIO_(fileIO) {}

    std::unique_ptr<ResourceObject> ModelLoader::load(const ResourceId& id, std::string_view virtualPath)
    {
        try
        {
            const auto key = canonicalizeResourceKey(virtualPath);
            const bool gltf = Path::hasExtension(key, "gltf");
            if (key.empty() || (!gltf && !Path::hasExtension(key, "obj")))
            {
                throw Error::Exception(Error::Code::IO, "Only static .gltf 2.0 and .obj models are supported");
            }
            const auto path = Path::join("assets", key);
            ImportDiagnostics diagnostics;
            Assimp::Importer importer;
            auto* io = new Detail::AssimpFileIO(fileIO_, path, gltf);
            importer.SetIOHandler(io);  // Importer 持有该数据；FileIO 仅在本次调用期间借用。
            importer.SetPropertyFloat(AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, 80.0f);
            const unsigned normalProcess = gltf ? aiProcess_GenNormals : aiProcess_GenSmoothNormals;
            const auto* scene =
                importer.ReadFile(path, aiProcess_Triangulate | aiProcess_ValidateDataStructure | normalProcess);
            if (!io->failure().empty())
                throw Error::Exception(Error::Code::IO, io->failure());
            if (!diagnostics.failure.empty())
                throw Error::Exception(Error::Code::IO, diagnostics.failure);
            if (!scene)
                throw Error::Exception(Error::Code::IO, importer.GetErrorString());
            if (scene->mNumAnimations != 0 || scene->mNumTextures != 0)
            {
                throw Error::Exception(Error::Code::IO, "Animation and embedded model images are unsupported");
            }

            std::vector<ModelMaterial> materials;
            materials.reserve(scene->mNumMaterials);
            for (unsigned i = 0; i < scene->mNumMaterials; ++i)
            {
                aiString name;
                scene->mMaterials[i]->Get(AI_MATKEY_NAME, name);
                materials.push_back(convertMaterial(*scene->mMaterials[i], i, gltf, Path::getParentPath(key),
                                                    io->hasObjDiffuseColor(name.C_Str())));
            }
            std::vector<MeshData> geometries;
            std::vector<ModelPart> parts;
            geometries.reserve(scene->mNumMeshes);
            parts.reserve(scene->mNumMeshes);
            for (unsigned i = 0; i < scene->mNumMeshes; ++i)
            {
                const auto& mesh = *scene->mMeshes[i];
                if (mesh.HasBones() || mesh.mNumAnimMeshes != 0)
                {
                    throw Error::Exception(
                        Error::Code::IO, fmt::format("Model geometry {} uses skin/Morph", i));
                }
                if (mesh.mMaterialIndex >= materials.size())
                {
                    throw Error::Exception(Error::Code::IO, fmt::format("Model geometry {} has invalid material", i));
                }
                if (materials[mesh.mMaterialIndex].texturePath && !mesh.HasTextureCoords(0))
                {
                    throw Error::Exception(Error::Code::IO, fmt::format("Model geometry {} material {} requires UV0", i,
                                                                        mesh.mMaterialIndex));
                }
                MeshData geometry;
                geometry.vertices.reserve(mesh.mNumVertices);
                for (unsigned v = 0; v < mesh.mNumVertices; ++v)
                {
                    const auto& position = mesh.mVertices[v];
                    std::array<float, 2> uv{};
                    if (mesh.HasTextureCoords(0))
                    {
                        // 两个导入器都提供 Assimp 的左下原点 UV；glTF 已反转源 V 坐标。
                        uv = {mesh.mTextureCoords[0][v].x, 1.0f - mesh.mTextureCoords[0][v].y};
                    }
                    std::array<float, 3> normal{};
                    if (mesh.HasNormals())
                    {
                        const auto& sourceNormal = mesh.mNormals[v];
                        normal = {sourceNormal.x, sourceNormal.y, sourceNormal.z};
                        // 用双精度长度保留很小或很大的有限法线；非有限值交给载荷边界拒绝。
                        const double length = std::hypot(double(normal[0]), double(normal[1]), double(normal[2]));
                        if (std::isfinite(length) && length > 0)
                            for (auto& component : normal)
                                component = static_cast<float>(component / length);
                    }
                    geometry.vertices.push_back({{position.x, position.y, position.z}, uv, normal});
                }
                for (unsigned f = 0; f < mesh.mNumFaces; ++f)
                {
                    const auto& face = mesh.mFaces[f];
                    if (face.mNumIndices != 3)
                    {
                        throw Error::Exception(Error::Code::IO,
                                               fmt::format("Model geometry {} is not a triangle list", i));
                    }
                    for (unsigned corner = 0; corner < 3; ++corner)
                    {
                        const unsigned index = face.mIndices[corner];
                        if (index >= mesh.mNumVertices)
                        {
                            throw Error::Exception(
                                Error::Code::IO, fmt::format("Model geometry {} index exceeds vertex range", i));
                        }
                        geometry.indices.push_back(static_cast<uint32_t>(index));
                    }
                }
                geometries.push_back(std::move(geometry));
                parts.push_back({i, mesh.mMaterialIndex});
            }

            std::vector<ModelNode> nodes;
            std::vector<const aiNode*> pending{scene->mRootNode};
            for (size_t i = 0; i < pending.size(); ++i)
            {
                const auto& node = *pending[i];
                ModelNode output;
                output.name = node.mName.C_Str();
                output.localTransform = convertMatrix(node.mTransformation);
                for (unsigned m = 0; m < node.mNumMeshes; ++m)
                    output.parts.push_back(node.mMeshes[m]);
                for (unsigned c = 0; c < node.mNumChildren; ++c)
                {
                    if (pending.size() > std::numeric_limits<uint32_t>::max())
                    {
                        throw Error::Exception(Error::Code::IO, "Model node count exceeds internal index range");
                    }
                    output.children.push_back(static_cast<uint32_t>(pending.size()));
                    pending.push_back(node.mChildren[c]);
                }
                nodes.push_back(std::move(output));
            }
            auto result = std::make_unique<ModelResource>(id, std::move(geometries), std::move(materials),
                                                          std::move(parts), std::move(nodes), 0);
            Log::Registry::get("resource")
                .debug("Imported model '{}': {} geometries, {} materials, {} nodes", path, result->geometries().size(),
                       result->materials().size(), result->nodes().size());
            return result;
        }
        catch (const Error::Exception& error)
        {
            throw Error::Exception(Error::Code::IO,
                                   fmt::format("ModelLoader 'assets/{}': {}", virtualPath, error.message()));
        }
        catch (const std::exception& error)
        {
            throw Error::Exception(Error::Code::IO,
                                   fmt::format("ModelLoader 'assets/{}': {}", virtualPath, error.what()));
        }
    }
}  // namespace Incubator::Resource
