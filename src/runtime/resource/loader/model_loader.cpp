#include "runtime/resource/loader/model_loader.h"
#include "runtime/resource/loader/detail/image_decode.h"

#include <assimp/GltfMaterial.h>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <assimp/DefaultLogger.hpp>
#include <assimp/Importer.hpp>
#include <assimp/LogStream.hpp>
#include <charconv>
#include <cmath>
#include <exception>
#include <glm/glm.hpp>
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

        ModelMaterial convertMaterial(const aiMaterial& source, uint32_t index, bool gltf, bool glb,
                                      std::string_view directory, bool objDiffuseSpecified,
                                      const auto& decodeEmbedded)
        {
            const auto reject = [index](std::string_view reason) {
                throw Error::Exception(Error::Code::IO, fmt::format("Model material {}: {}", index, reason));
            };
            if (!gltf)
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
                aiString mode("OPAQUE");
                source.Get(AI_MATKEY_GLTF_ALPHAMODE, mode);
                const std::string_view alphaMode(mode.C_Str(), mode.length);
                if (alphaMode == "MASK")
                    result.alphaMode = AlphaMode::Mask;
                else if (alphaMode == "BLEND")
                    result.alphaMode = AlphaMode::Blend;
                else if (alphaMode != "OPAQUE")
                    reject("Invalid glTF alphaMode");
                source.Get(AI_MATKEY_GLTF_ALPHACUTOFF, result.alphaCutoff);
                source.Get(AI_MATKEY_TWOSIDED, result.doubleSided);
                aiColor4D emissive(0.0f, 0.0f, 0.0f, 1.0f);
                source.Get(AI_MATKEY_COLOR_EMISSIVE, emissive);
                result.emissiveFactor = {emissive.r, emissive.g, emissive.b};
            }
            aiColor4D color(1.0f, 1.0f, 1.0f, 1.0f);
            if (gltf)
            {
                source.Get(AI_MATKEY_BASE_COLOR, color);
                result.baseAlpha = color.a;
            }
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

            const auto parseTextureSlot = [&](aiTextureType textureType, std::string_view label,
                                              std::optional<std::string>& external,
                                              std::optional<uint32_t>& embedded) {
                const unsigned count = source.GetTextureCount(textureType);
                if (count > 1)
                    reject(fmt::format("Multiple {} textures are unsupported", label));
                if (count == 0)
                    return;
                aiString path;
                aiTextureMapping mapping = aiTextureMapping_UV;
                unsigned uv = 0;
                if (source.GetTexture(textureType, 0, &path, &mapping, &uv) != aiReturn_SUCCESS ||
                    mapping != aiTextureMapping_UV || uv != 0)
                {
                    reject(fmt::format("{} texture requires UV0", label));
                }
                aiUVTransform transform;
                if (source.Get(AI_MATKEY_UVTRANSFORM(textureType, 0), transform) == aiReturn_SUCCESS)
                {
                    reject(fmt::format("{} UV transform is unsupported", label));
                }
                if (glb)
                {
                    if (path.length < 2 || path.data[0] != '*')
                        reject("GLB material requires an embedded image reference");
                    embedded = decodeEmbedded(std::string_view(path.C_Str(), path.length).substr(1));
                    return;
                }
                if (path.length == 0 || path.data[0] == '*')
                    reject("Embedded model image is unsupported");
                std::string resolved = Detail::modelDependencyPath(directory, path.C_Str(), gltf);
                if (!Detail::imageFormatFromPath(resolved))
                    reject("Only external PNG or JPEG images are supported");
                external = std::move(resolved);
            };

            parseTextureSlot(gltf ? aiTextureType_BASE_COLOR : aiTextureType_DIFFUSE, "Base color",
                             result.texturePath, result.embeddedTextureIndex);
            if (gltf)
            {
                parseTextureSlot(aiTextureType_NORMALS, "Normal", result.normalTexturePath,
                                 result.embeddedNormalTextureIndex);
                parseTextureSlot(aiTextureType_GLTF_METALLIC_ROUGHNESS, "Metallic roughness",
                                 result.metallicRoughnessTexturePath, result.embeddedMetallicRoughnessTextureIndex);
                parseTextureSlot(aiTextureType_EMISSIVE, "Emissive", result.emissiveTexturePath,
                                 result.embeddedEmissiveTextureIndex);

                result.normalScale = 1.0f;
                source.Get(AI_MATKEY_GLTF_TEXTURE_SCALE(aiTextureType_NORMALS, 0), result.normalScale);
                if (!std::isfinite(result.normalScale) || result.normalScale < 0.0f)
                {
                    reject("Requires finite nonnegative normal scale");
                }

                result.metallic = 1.0f;
                source.Get(AI_MATKEY_METALLIC_FACTOR, result.metallic);
                if (!std::isfinite(result.metallic) || result.metallic < 0.0f || result.metallic > 1.0f)
                {
                    reject("Requires finite metallic factor in [0, 1]");
                }

                result.roughness = 1.0f;
                source.Get(AI_MATKEY_ROUGHNESS_FACTOR, result.roughness);
                if (!std::isfinite(result.roughness) || result.roughness < 0.0f || result.roughness > 1.0f)
                {
                    reject("Requires finite roughness factor in [0, 1]");
                }
            }
            else
            {
                result.normalScale = 1.0f;
                result.metallic = 0.0f;
                result.roughness = 0.45f;
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

    ModelLoader::ModelLoader(AssetSource& source) : source_(source) {}

    std::unique_ptr<ResourceObject> ModelLoader::load(const ResourceId& id, std::string_view virtualPath)
    {
        try
        {
            source_.resolve(virtualPath);
            const auto key = canonicalizeResourceKey(virtualPath);
            const bool glb = Path::hasExtension(key, "glb");
            const bool gltf = glb || Path::hasExtension(key, "gltf");
            if (key.empty() || (!gltf && !Path::hasExtension(key, "obj")))
            {
                throw Error::Exception(Error::Code::IO, "Only static .gltf 2.0, .glb and .obj models are supported");
            }
            const auto& path = key;
            ImportDiagnostics diagnostics;
            Assimp::Importer importer;
            auto* io = new Detail::AssimpFileIO(source_, path, gltf);
            importer.SetIOHandler(io);  // Importer 持有该数据；AssetSource 仅在本次调用期间借用。
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
            if (scene->mNumAnimations != 0 || (!glb && scene->mNumTextures != 0))
            {
                throw Error::Exception(Error::Code::IO, "Animation and embedded model images are unsupported");
            }

            std::vector<EmbeddedTexture> embeddedTextures;
            std::vector<std::optional<uint32_t>> embeddedIndices(scene->mNumTextures);
            const auto decodeEmbedded = [&](std::string_view reference) -> uint32_t {
                unsigned index = 0;
                const auto end = reference.data() + reference.size();
                const auto [parsed, error] = std::from_chars(reference.data(), end, index);
                if (error != std::errc{} || parsed != end || index >= scene->mNumTextures || !scene->mTextures[index])
                    throw Error::Exception(Error::Code::IO, "Invalid GLB embedded texture reference");
                auto& mapped = embeddedIndices[index];
                if (!mapped)
                {
                    const auto& texture = *scene->mTextures[index];
                    if (texture.mHeight != 0 || !texture.pcData)
                        throw Error::Exception(Error::Code::IO, "GLB requires encoded embedded texture bytes");
                    const std::string_view hint(texture.achFormatHint, 3);
                    if (hint != "png" && hint != "jpg")
                        throw Error::Exception(Error::Code::IO, "Unsupported GLB embedded texture format");
                    const auto* data = reinterpret_cast<const uint8_t*>(texture.pcData);
                    auto image = Detail::decodeRgba8({data, texture.mWidth},
                                                     hint == "png" ? Detail::ImageFormat::Png
                                                                   : Detail::ImageFormat::Jpeg);
                    mapped = static_cast<uint32_t>(embeddedTextures.size());
                    embeddedTextures.push_back({image.width, image.height, std::move(image.pixels)});
                }
                return *mapped;
            };

            std::vector<ModelMaterial> materials;
            materials.reserve(scene->mNumMaterials);
            for (unsigned i = 0; i < scene->mNumMaterials; ++i)
            {
                aiString name;
                scene->mMaterials[i]->Get(AI_MATKEY_NAME, name);
                materials.push_back(convertMaterial(*scene->mMaterials[i], i, gltf, glb, Path::getParentPath(key),
                                                    io->hasObjDiffuseColor(name.C_Str()), decodeEmbedded));
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
                    throw Error::Exception(Error::Code::IO, fmt::format("Model geometry {} uses skin/Morph", i));
                }
                if (mesh.mMaterialIndex >= materials.size())
                {
                    throw Error::Exception(Error::Code::IO, fmt::format("Model geometry {} has invalid material", i));
                }
                const auto& material = materials[mesh.mMaterialIndex];
                if ((material.texturePath || material.normalTexturePath || material.metallicRoughnessTexturePath ||
                     material.embeddedTextureIndex || material.embeddedNormalTextureIndex ||
                     material.embeddedMetallicRoughnessTextureIndex || material.emissiveTexturePath ||
                     material.embeddedEmissiveTextureIndex) && !mesh.HasTextureCoords(0))
                {
                    throw Error::Exception(Error::Code::IO, fmt::format("Model geometry {} material {} requires UV0", i,
                                                                        mesh.mMaterialIndex));
                }
                MeshData geometry;
                geometry.vertices.reserve(mesh.mNumVertices);
                const bool hasSourceTangents = mesh.HasTangentsAndBitangents();
                const auto orthonormalizeTangent = [](const glm::dvec3& normal, const glm::dvec3& rawTangent,
                                                      const glm::dvec3& bitangent) -> std::array<float, 4> {
                    const double nLen2 = glm::dot(normal, normal);
                    if (!std::isfinite(nLen2) || nLen2 <= 0.0)
                        return {};
                    const glm::dvec3 ortho = rawTangent - normal * glm::dot(normal, rawTangent);
                    const double len = glm::length(ortho);
                    if (!std::isfinite(len) || len <= 1e-8)
                        return {};
                    const glm::dvec3 tangent = ortho / len;
                    const float handedness = glm::dot(glm::cross(normal, tangent), bitangent) < 0.0 ? -1.0f : 1.0f;
                    return {static_cast<float>(tangent.x), static_cast<float>(tangent.y),
                            static_cast<float>(tangent.z), handedness};
                };
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
                    std::array<float, 4> tangent{};
                    if (hasSourceTangents)
                    {
                        const auto& srcT = mesh.mTangents[v];
                        const auto& srcB = mesh.mBitangents[v];
                        if (!std::isfinite(srcT.x) || !std::isfinite(srcT.y) || !std::isfinite(srcT.z) ||
                            !std::isfinite(srcB.x) || !std::isfinite(srcB.y) || !std::isfinite(srcB.z))
                        {
                            throw Error::Exception(
                                Error::Code::IO, fmt::format("Model geometry {} has non-finite tangent/bitangent", i));
                        }
                        tangent = orthonormalizeTangent(glm::dvec3(normal[0], normal[1], normal[2]),
                                                        glm::dvec3(srcT.x, srcT.y, srcT.z),
                                                        glm::dvec3(srcB.x, srcB.y, srcB.z));
                    }
                    geometry.vertices.push_back({{position.x, position.y, position.z}, uv, normal, tangent});
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
                            throw Error::Exception(Error::Code::IO,
                                                   fmt::format("Model geometry {} index exceeds vertex range", i));
                        }
                        geometry.indices.push_back(static_cast<uint32_t>(index));
                    }
                }
                if (!hasSourceTangents && mesh.HasTextureCoords(0) && mesh.HasNormals())
                {
                    std::vector<glm::dvec3> accT(geometry.vertices.size(), glm::dvec3(0.0));
                    std::vector<glm::dvec3> accB(geometry.vertices.size(), glm::dvec3(0.0));
                    for (size_t idx = 0; idx + 2 < geometry.indices.size(); idx += 3)
                    {
                        const uint32_t i0 = geometry.indices[idx];
                        const uint32_t i1 = geometry.indices[idx + 1];
                        const uint32_t i2 = geometry.indices[idx + 2];
                        const auto& v0 = geometry.vertices[i0];
                        const auto& v1 = geometry.vertices[i1];
                        const auto& v2 = geometry.vertices[i2];
                        const glm::dvec3 dp1(double(v1.position[0]) - v0.position[0],
                                             double(v1.position[1]) - v0.position[1],
                                             double(v1.position[2]) - v0.position[2]);
                        const glm::dvec3 dp2(double(v2.position[0]) - v0.position[0],
                                             double(v2.position[1]) - v0.position[1],
                                             double(v2.position[2]) - v0.position[2]);
                        const double du1 = double(v1.uv[0]) - v0.uv[0];
                        const double dv1 = double(v1.uv[1]) - v0.uv[1];
                        const double du2 = double(v2.uv[0]) - v0.uv[0];
                        const double dv2 = double(v2.uv[1]) - v0.uv[1];
                        const double det = du1 * dv2 - du2 * dv1;
                        if (!std::isfinite(det) || std::abs(det) <= 1e-10)
                            continue;
                        const double invDet = 1.0 / det;
                        const glm::dvec3 faceT = (dp1 * dv2 - dp2 * dv1) * invDet;
                        const glm::dvec3 faceB = (dp2 * du1 - dp1 * du2) * invDet;
                        if (!std::isfinite(faceT.x) || !std::isfinite(faceT.y) || !std::isfinite(faceT.z) ||
                            !std::isfinite(faceB.x) || !std::isfinite(faceB.y) || !std::isfinite(faceB.z))
                        {
                            continue;
                        }
                        for (const uint32_t cornerIndex : {i0, i1, i2})
                        {
                            accT[cornerIndex] += faceT;
                            accB[cornerIndex] += faceB;
                        }
                    }
                    for (size_t v = 0; v < geometry.vertices.size(); ++v)
                    {
                        const auto& n = geometry.vertices[v].normal;
                        geometry.vertices[v].tangent =
                            orthonormalizeTangent(glm::dvec3(n[0], n[1], n[2]), accT[v], accB[v]);
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
                                                          std::move(parts), std::move(nodes), 0,
                                                          std::move(embeddedTextures));
            Log::Registry::get("resource")
                .debug("Imported model '{}': {} geometries, {} materials, {} nodes", path, result->geometries().size(),
                       result->materials().size(), result->nodes().size());
            return result;
        }
        catch (const Error::Exception& error)
        {
            throw Error::Exception(Error::Code::IO, fmt::format("ModelLoader '{}': {}", virtualPath, error.message()));
        }
        catch (const std::exception& error)
        {
            throw Error::Exception(Error::Code::IO, fmt::format("ModelLoader '{}': {}", virtualPath, error.what()));
        }
    }
}  // namespace Incubator::Resource
