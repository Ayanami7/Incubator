#include "runtime/resource/loader/model_resource.h"

#include <gtest/gtest.h>

#include <array>
#include <glm/ext/matrix_transform.hpp>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "runtime/core/error/error.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource;

    static_assert(std::is_final_v<ModelResource>);
    static_assert(std::is_same_v<decltype(std::declval<ModelResource&>().geometries()), const std::vector<MeshData>&>);
    static_assert(
        std::is_same_v<decltype(std::declval<ModelResource&>().materials()), const std::vector<ModelMaterial>&>);
    static_assert(std::is_same_v<decltype(std::declval<ModelResource&>().parts()), const std::vector<ModelPart>&>);
    static_assert(std::is_same_v<decltype(std::declval<ModelResource&>().nodes()), const std::vector<ModelNode>&>);

    MeshData triangle()
    {
        return {{{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
                 {{1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
                 {{0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}}},
                {0, 1, 2}};
    }

    struct ModelInput
    {
        std::vector<MeshData> geometries{triangle()};
        std::vector<ModelMaterial> materials{ModelMaterial{}};
        std::vector<ModelPart> parts{{0, 0}};
        std::vector<ModelNode> nodes{{"root", glm::mat4(1.0f), {}, {0}}};
        uint32_t rootNode = 0;
    };

    const ResourceId MODEL_ID = ResourceId::fromPath(ResourceType::MODEL, "models/manual.gltf");

    std::unique_ptr<ModelResource> makeModel(ModelInput input)
    {
        return std::make_unique<ModelResource>(MODEL_ID, std::move(input.geometries), std::move(input.materials),
                                               std::move(input.parts), std::move(input.nodes), input.rootNode);
    }

    void expectInvalid(const ModelInput& input, std::string_view reason)
    {
        try
        {
            makeModel(input);
            FAIL() << "Invalid model was accepted";
        }
        catch (const Error::Exception& error)
        {
            EXPECT_EQ(error.code(), Error::Code::CONFIG);
            EXPECT_NE(error.message().find(reason), std::string::npos) << error.message();
        }
    }

    ModelInput composedModel()
    {
        ModelInput input;
        input.geometries.push_back(triangle());
        input.geometries[1].vertices[1].position[0] = 2.0f;
        input.materials = {{"textures/方向😀.png", {0.5f, 1.0f, 0.5f}}, {std::nullopt, {0.25f, 0.5f, 1.0f}}};
        input.parts = {{0, 0}, {1, 1}, {0, 1}};
        input.rootNode = 2;
        const auto root = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
        const auto parent = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        const auto leaf = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f)) *
                          glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, 1.0f));
        input.nodes = {{"parent", parent, {1}, {}},
                       {"leaf", leaf, {}, {0, 1}},
                       {"source-root", root, {0, 3}, {}},
                       {"shared", glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, 0.0f, 0.0f)), {}, {2}}};
        return input;
    }

    void expectPoint(const glm::vec4& actual, const std::array<float, 3>& expected)
    {
        for (int i = 0; i < 3; ++i)
        {
            EXPECT_NEAR(actual[i], expected[i], 0.00001f);
        }
        EXPECT_FLOAT_EQ(actual.w, 1.0f);
    }

    TEST(ModelResourceTest, UsesModelIdentityAndDefaultOpaqueWhiteDescription)
    {
        const auto model = makeModel({});
        EXPECT_EQ(model->id(), MODEL_ID);
        EXPECT_EQ(model->type(), ResourceType::MODEL);
        EXPECT_EQ(model->state(), ResourceState::UNLOADED);
        EXPECT_EQ(model->rootNode(), 0u);
        ASSERT_EQ(model->materials().size(), 1u);
        EXPECT_FALSE(model->materials()[0].texturePath.has_value());
        EXPECT_EQ(model->materials()[0].color, (std::array<float, 3>{1.0f, 1.0f, 1.0f}));
        EXPECT_EQ(model->geometries()[0].indices, (std::vector<uint16_t>{0, 1, 2}));
        expectPoint(model->nodes()[0].localTransform * glm::vec4(1.0f), {1.0f, 1.0f, 1.0f});
    }

    TEST(ModelResourceTest, PreservesPartsMaterialsSourceRootAndSharedGeometry)
    {
        const auto model = makeModel(composedModel());
        ASSERT_EQ(model->geometries().size(), 2u);
        ASSERT_EQ(model->materials().size(), 2u);
        ASSERT_EQ(model->parts().size(), 3u);
        ASSERT_EQ(model->nodes().size(), 4u);
        EXPECT_EQ(model->rootNode(), 2u);
        EXPECT_EQ(model->nodes()[2].name, "source-root");
        EXPECT_EQ(model->nodes()[2].children, (std::vector<uint32_t>{0, 3}));
        EXPECT_EQ(model->nodes()[1].parts, (std::vector<uint32_t>{0, 1}));
        const auto leafPart = model->parts()[model->nodes()[1].parts[0]];
        const auto sharedPart = model->parts()[model->nodes()[3].parts[0]];
        EXPECT_EQ(leafPart.geometry, sharedPart.geometry);
        EXPECT_NE(leafPart.material, sharedPart.material);
        EXPECT_EQ(*model->materials()[leafPart.material].texturePath, "textures/方向😀.png");
        EXPECT_FALSE(model->materials()[sharedPart.material].texturePath.has_value());
    }

    TEST(ModelResourceTest, CopiesAllNestedPayloadsWithoutBorrowingSource)
    {
        auto input = composedModel();
        ModelResource model(MODEL_ID, input.geometries, input.materials, input.parts, input.nodes, input.rootNode);
        input.geometries[0].vertices[0].position[0] = 99.0f;
        input.geometries[0].vertices[1].uv[0] = 99.0f;
        input.geometries[0].indices[0] = 2;
        *input.materials[0].texturePath = "changed.png";
        input.materials[0].color[0] = 0.0f;
        input.parts[0].material = 1;
        input.nodes[2].name = "changed";
        input.nodes[2].localTransform = glm::mat4(0.0f);
        input.nodes[2].children.clear();
        input.nodes[1].parts.clear();
        input = {};
        EXPECT_FLOAT_EQ(model.geometries()[0].vertices[0].position[0], 0.0f);
        EXPECT_FLOAT_EQ(model.geometries()[0].vertices[1].uv[0], 1.0f);
        EXPECT_EQ(model.geometries()[0].indices[0], 0u);
        EXPECT_EQ(*model.materials()[0].texturePath, "textures/方向😀.png");
        EXPECT_FLOAT_EQ(model.materials()[0].color[0], 0.5f);
        EXPECT_EQ(model.parts()[0].material, 0u);
        EXPECT_EQ(model.nodes()[2].name, "source-root");
        EXPECT_EQ(model.nodes()[2].children, (std::vector<uint32_t>{0, 3}));
        EXPECT_EQ(model.nodes()[1].parts, (std::vector<uint32_t>{0, 1}));
        expectPoint(model.nodes()[2].localTransform * glm::vec4(0, 0, 0, 1), {10.0f, 0.0f, 0.0f});
    }

    TEST(ModelResourceTest, OwnsMovedPayloadAfterAllBuilderStorageIsDestroyed)
    {
        std::unique_ptr<ModelResource> model;
        {
            auto input = composedModel();
            model = makeModel(std::move(input));
        }
        EXPECT_EQ(model->nodes()[2].children, (std::vector<uint32_t>{0, 3}));
        EXPECT_EQ(model->geometries()[1].indices, (std::vector<uint16_t>{0, 1, 2}));
        EXPECT_FLOAT_EQ(model->geometries()[1].vertices[1].position[0], 2.0f);
        EXPECT_EQ(*model->materials()[0].texturePath, "textures/方向😀.png");
    }

    TEST(ModelResourceTest, NonIdentityRootAndParentChildTransformsHaveExplicitExpectedProducts)
    {
        const auto model = makeModel(composedModel());
        const auto& nodes = model->nodes();
        const auto leafWorld =
            nodes[model->rootNode()].localTransform * nodes[0].localTransform * nodes[1].localTransform;
        expectPoint(leafWorld * glm::vec4(0, 0, 0, 1), {10.0f, 2.0f, 0.0f});
        expectPoint(leafWorld * glm::vec4(1, 1, 0, 1), {7.0f, 4.0f, 0.0f});
        expectPoint(nodes[2].localTransform * nodes[3].localTransform * glm::vec4(0, 0, 0, 1), {9.0f, 0.0f, 0.0f});
        const auto instance = glm::translate(glm::mat4(1.0f), glm::vec3(0, 5, 0));
        expectPoint(instance * leafWorld * glm::vec4(1, 1, 0, 1), {7.0f, 9.0f, 0.0f});
    }

    TEST(ModelResourceTest, AllowsRootOnlyModelWithoutGeometryOrMaterials)
    {
        ModelInput input;
        input.geometries.clear();
        input.materials.clear();
        input.parts.clear();
        input.nodes[0].parts.clear();
        const auto model = makeModel(input);
        EXPECT_TRUE(model->geometries().empty());
        EXPECT_TRUE(model->materials().empty());
        EXPECT_TRUE(model->parts().empty());
        EXPECT_EQ(model->nodes().size(), 1u);
    }

    TEST(ModelResourceTest, RejectsMissingAndOutOfRangeRoot)
    {
        ModelInput input;
        input.rootNode = 1;
        expectInvalid(input, "root node");
        input.rootNode = std::numeric_limits<uint32_t>::max();
        expectInvalid(input, "root node");
        input.nodes.clear();
        input.rootNode = 0;
        expectInvalid(input, "root node");
    }

    TEST(ModelResourceTest, RejectsEmptyGeometryAndVertexCountAboveUint16Range)
    {
        ModelInput input;
        input.geometries[0].vertices.clear();
        expectInvalid(input, "geometry 0");
        input.geometries[0].vertices.resize(MeshData::MAX_VERTEX_COUNT + 1);
        expectInvalid(input, "65536");
    }

    TEST(ModelResourceTest, RejectsEmptyAndIncompleteTriangleIndexLists)
    {
        for (size_t count : {0u, 1u, 2u, 4u, 5u})
        {
            SCOPED_TRACE(count);
            ModelInput input;
            input.geometries[0].indices.resize(count, 0);
            expectInvalid(input, "triangle index list");
        }
    }

    TEST(ModelResourceTest, RejectsIndicesOutsideGeometryVertexRange)
    {
        for (uint16_t index : {uint16_t{3}, std::numeric_limits<uint16_t>::max()})
        {
            ModelInput input;
            input.geometries[0].indices[2] = index;
            expectInvalid(input, "vertex range");
        }
    }

    TEST(ModelResourceTest, AcceptsExactly65536VerticesAndHighestUint16Index)
    {
        ModelInput input;
        input.geometries[0].vertices.resize(MeshData::MAX_VERTEX_COUNT);
        input.geometries[0].indices = {0, 65534, 65535};
        const auto model = makeModel(std::move(input));
        EXPECT_EQ(model->geometries()[0].vertices.size(), 65536u);
        EXPECT_EQ(model->geometries()[0].indices.back(), 65535u);
    }

    TEST(ModelResourceTest, RejectsNonFinitePositionsAndUv)
    {
        for (float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity()})
        {
            for (size_t component = 0; component < 3; ++component)
            {
                ModelInput input;
                input.geometries[0].vertices[0].position[component] = value;
                expectInvalid(input, "position");
            }
            for (size_t component = 0; component < 2; ++component)
            {
                ModelInput input;
                input.geometries[0].vertices[0].uv[component] = value;
                expectInvalid(input, "UV");
            }
        }
    }

    TEST(ModelResourceTest, AcceptsFiniteOutOfRangeUvAndZeroScaleWithoutRepair)
    {
        ModelInput input;
        input.geometries[0].vertices[0].uv = {-2.0f, 3.0f};
        input.nodes[0].localTransform = glm::scale(glm::mat4(1.0f), glm::vec3(0.0f, -1.0f, 1.0f));
        const auto model = makeModel(input);
        EXPECT_EQ(model->geometries()[0].vertices[0].uv, (std::array<float, 2>{-2.0f, 3.0f}));
        EXPECT_FLOAT_EQ(model->nodes()[0].localTransform[0][0], 0.0f);
        EXPECT_FLOAT_EQ(model->nodes()[0].localTransform[1][1], -1.0f);
    }

    TEST(ModelResourceTest, RejectsNonFiniteOrOutOfRangeMaterialColors)
    {
        for (float value : {-0.1f, 1.1f, std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()})
        {
            for (size_t component = 0; component < 3; ++component)
            {
                ModelInput input;
                input.materials[0].color[component] = value;
                expectInvalid(input, "material 0");
            }
        }
    }

    TEST(ModelResourceTest, CanonicalizesResolvedTextureVirtualPaths)
    {
        ModelInput input;
        input.materials[0].texturePath = "Textures\\parent/../方向😀.PNG";
        const auto model = makeModel(input);
        EXPECT_EQ(*model->materials()[0].texturePath, "textures/方向😀.png");
    }

    TEST(ModelResourceTest, RejectsPresentButInvalidTexturePathsInsteadOfUsingSolidColor)
    {
        for (const char* path : {"", ".", "../outside.png", "C:/image.png", "C:image.png", "//server/image.png"})
        {
            SCOPED_TRACE(path);
            ModelInput input;
            input.materials[0].texturePath = path;
            expectInvalid(input, "texture virtual path");
        }
    }

    TEST(ModelResourceTest, RejectsInvalidGeometryAndMaterialPartReferences)
    {
        ModelInput input;
        input.parts[0].geometry = 1;
        expectInvalid(input, "part 0");
        input.parts[0].geometry = 0;
        input.parts[0].material = std::numeric_limits<uint32_t>::max();
        expectInvalid(input, "part 0");
    }

    TEST(ModelResourceTest, RejectsInvalidNodePartReferences)
    {
        ModelInput input;
        input.nodes[0].parts.push_back(1);
        expectInvalid(input, "part reference 1");
    }

    TEST(ModelResourceTest, RejectsOutOfRangeChildReferences)
    {
        ModelInput input;
        input.nodes[0].children = {std::numeric_limits<uint32_t>::max()};
        expectInvalid(input, "child reference");
    }

    TEST(ModelResourceTest, RejectsDuplicateChildInSameParent)
    {
        ModelInput input;
        input.nodes.emplace_back();
        input.nodes[0].children = {1, 1};
        expectInvalid(input, "repeated parent");
    }

    TEST(ModelResourceTest, RejectsMultipleParents)
    {
        ModelInput input;
        input.nodes.resize(3);
        input.nodes[0].children = {1, 2};
        input.nodes[1].children = {2};
        expectInvalid(input, "repeated parent");
    }

    TEST(ModelResourceTest, RejectsRootBackEdgesAndReachableCycles)
    {
        ModelInput input;
        input.nodes[0].children = {0};
        expectInvalid(input, "root back-edge");
        input.nodes.resize(2);
        input.nodes[0].children = {1};
        input.nodes[1].children = {0};
        expectInvalid(input, "root back-edge");
        input.nodes[1].children = {1};
        expectInvalid(input, "repeated parent");
    }

    TEST(ModelResourceTest, RejectsDisconnectedNodes)
    {
        ModelInput input;
        input.nodes.emplace_back();
        expectInvalid(input, "disconnected");
    }

    TEST(ModelResourceTest, RejectsDisconnectedCyclesEvenWithOneParentPerNode)
    {
        ModelInput input;
        input.nodes.resize(3);
        input.nodes[1].children = {2};
        input.nodes[2].children = {1};
        expectInvalid(input, "cycle");
    }

    TEST(ModelResourceTest, RejectsNonFiniteLocalMatrixElements)
    {
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
            {
                ModelInput input;
                input.nodes[0].localTransform[column][row] = std::numeric_limits<float>::quiet_NaN();
                expectInvalid(input, "local transform");
            }
        }
    }

    TEST(ModelResourceTest, ValidatesDeepHierarchyWithoutRecursiveTraversal)
    {
        ModelInput input;
        input.nodes.resize(4096);
        for (uint32_t i = 1; i < input.nodes.size(); ++i)
        {
            input.nodes[i - 1].children.push_back(i);
        }
        const auto model = makeModel(std::move(input));
        EXPECT_EQ(model->nodes().size(), 4096u);
        EXPECT_TRUE(model->nodes().back().children.empty());
    }
}  // namespace
