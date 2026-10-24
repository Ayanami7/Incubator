#include "runtime/resource/loader/model_resource.h"

#include <cmath>
#include <limits>
#include <utility>

#include "runtime/core/error/error.h"

namespace Incubator
{
    namespace Resource
    {
        namespace
        {
            Math::Aabb validateGeometry(const MeshData& mesh, size_t geometry)
            {
                if (mesh.vertices.empty())
                {
                    throw Error::Exception(Error::Code::CONFIG,
                                           fmt::format("Model geometry {} requires a nonempty vertex array", geometry));
                }
                if (mesh.indices.empty() || mesh.indices.size() % 3 != 0 ||
                    mesh.indices.size() > std::numeric_limits<uint32_t>::max())
                {
                    throw Error::Exception(
                        Error::Code::CONFIG,
                        fmt::format("Model geometry {} requires a uint32-sized triangle index list", geometry));
                }
                for (uint32_t index : mesh.indices)
                {
                    if (index >= mesh.vertices.size())
                    {
                        throw Error::Exception(
                            Error::Code::CONFIG,
                            fmt::format("Model geometry {} index {} exceeds its vertex range", geometry, index));
                    }
                }
                const auto& first = mesh.vertices.front().position;
                Math::Aabb bounds{glm::vec3(first[0], first[1], first[2]), glm::vec3(first[0], first[1], first[2])};
                for (const auto& vertex : mesh.vertices)
                {
                    for (float value : vertex.position)
                    {
                        if (!std::isfinite(value))
                        {
                            throw Error::Exception(Error::Code::CONFIG,
                                                   fmt::format("Model geometry {} has non-finite position", geometry));
                        }
                    }
                    bounds.include(glm::vec3(vertex.position[0], vertex.position[1], vertex.position[2]));
                    for (float value : vertex.uv)
                    {
                        if (!std::isfinite(value))
                        {
                            throw Error::Exception(Error::Code::CONFIG,
                                                   fmt::format("Model geometry {} has non-finite UV", geometry));
                        }
                    }
                    for (float value : vertex.normal)
                    {
                        if (!std::isfinite(value))
                        {
                            throw Error::Exception(Error::Code::CONFIG,
                                                   fmt::format("Model geometry {} has non-finite normal", geometry));
                        }
                    }
                }
                return bounds;
            }
        }  // namespace

        ModelResource::ModelResource(ResourceId id, std::vector<MeshData> geometries,
                                     std::vector<ModelMaterial> materials, std::vector<ModelPart> parts,
                                     std::vector<ModelNode> nodes, uint32_t rootNode)
            : ResourceObject(id, ResourceType::MODEL),
              geometries_(std::move(geometries)),
              materials_(std::move(materials)),
              parts_(std::move(parts)),
              nodes_(std::move(nodes)),
              rootNode_(rootNode)
        {
            geometryBounds_.reserve(geometries_.size());
            for (size_t i = 0; i < geometries_.size(); ++i)
            {
                geometryBounds_.push_back(validateGeometry(geometries_[i], i));
            }
            for (size_t i = 0; i < materials_.size(); ++i)
            {
                auto& material = materials_[i];
                for (float value : material.color)
                {
                    if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
                    {
                        throw Error::Exception(
                            Error::Code::CONFIG,
                            fmt::format("Model material {} requires finite linear RGB in [0, 1]", i));
                    }
                }
                if (material.texturePath)
                {
                    auto key = canonicalizeResourceKey(*material.texturePath);
                    if (key.empty() || key == ".")
                    {
                        throw Error::Exception(Error::Code::CONFIG,
                                               fmt::format("Model material {} has an invalid texture virtual path", i));
                    }
                    material.texturePath = std::move(key);
                }
            }
            for (size_t i = 0; i < parts_.size(); ++i)
            {
                if (parts_[i].geometry >= geometries_.size() || parts_[i].material >= materials_.size())
                {
                    throw Error::Exception(
                        Error::Code::CONFIG,
                        fmt::format("Model part {} has an invalid geometry or material reference", i));
                }
            }
            if (rootNode_ >= nodes_.size())
            {
                throw Error::Exception(Error::Code::CONFIG, "Model requires a valid root node");
            }

            std::vector<bool> hasParent(nodes_.size(), false);
            for (size_t i = 0; i < nodes_.size(); ++i)
            {
                const auto& node = nodes_[i];
                for (int column = 0; column < 4; ++column)
                {
                    for (int row = 0; row < 4; ++row)
                    {
                        if (!std::isfinite(node.localTransform[column][row]))
                        {
                            throw Error::Exception(Error::Code::CONFIG,
                                                   fmt::format("Model node {} has a non-finite local transform", i));
                        }
                    }
                }
                for (uint32_t part : node.parts)
                {
                    if (part >= parts_.size())
                    {
                        throw Error::Exception(Error::Code::CONFIG,
                                               fmt::format("Model node {} has an invalid part reference {}", i, part));
                    }
                }
                for (uint32_t child : node.children)
                {
                    if (child >= nodes_.size())
                    {
                        throw Error::Exception(
                            Error::Code::CONFIG,
                            fmt::format("Model node {} has an invalid child reference {}", i, child));
                    }
                    if (child == rootNode_ || hasParent[child])
                    {
                        throw Error::Exception(
                            Error::Code::CONFIG,
                            fmt::format("Model node {} has a root back-edge or repeated parent", child));
                    }
                    hasParent[child] = true;
                }
            }

            // 每个节点最多一个父节点，且没有边指向根节点时，从根节点可达的部分构成一棵树。
            // 最终节点数校验会拒绝不连通的部分，包括独立的环。
            size_t reached = 0;
            std::vector<uint32_t> pending{rootNode_};
            while (!pending.empty())
            {
                const uint32_t node = pending.back();
                pending.pop_back();
                ++reached;
                pending.insert(pending.end(), nodes_[node].children.begin(), nodes_[node].children.end());
            }
            if (reached != nodes_.size())
            {
                throw Error::Exception(Error::Code::CONFIG, "Model hierarchy is disconnected or contains a cycle");
            }
        }
    }  // namespace Resource
}  // namespace Incubator
