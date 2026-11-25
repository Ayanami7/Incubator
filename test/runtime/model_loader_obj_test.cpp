#include "model_loader_fixture.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource;
    using namespace Incubator::Test;
    namespace fs = std::filesystem;


    TEST_F(ModelLoaderTest, ObjMtlLoadsMultiplePartsAndTriangulatesQuad)
    {
        const auto* model = load(OBJ);
        ASSERT_NE(model, nullptr) << messages_.str();
        ASSERT_EQ(model->geometries().size(), 2u);
        EXPECT_EQ(model->geometries()[0].indices.size(), 6u);
        EXPECT_EQ(model->geometries()[1].indices.size(), 3u);
        EXPECT_EQ(*model->materials()[model->parts()[0].material].texturePath, "textures/direction_rgba.png");
        EXPECT_EQ(model->materials()[model->parts()[1].material].color, (std::array<float, 3>{0.5f, 1.0f, 0.5f}));
        EXPECT_EQ(node(*model, "DirectionQuad").parts.size(), 1u);
        expectClosed();
    }
    struct MtlCase
    {
        std::string name;
        std::string fields;
        bool accepted;
    };
    void PrintTo(const MtlCase& value, std::ostream* output)
    {
        *output << value.name;
    }
    class ModelObjMaterialTest : public ModelLoaderTest, public ::testing::WithParamInterface<MtlCase>
    {};
    TEST_P(ModelObjMaterialTest, UsesFixedAssimpOpacityOrderAndRejectsUnsupportedMaps)
    {
        const auto& value = GetParam();
        write("models/minimal_obj/materials/model.mtl",
              "newmtl Direction\n" + value.fields + "\nnewmtl Solid\nKd 1 1 1\n");
        if (value.accepted)
        {
            const auto* model = load(OBJ);
            ASSERT_NE(model, nullptr) << messages_.str();
            const float color = value.name == "explicit_gray" ? 0.6f : 1.0f;
            EXPECT_EQ(model->materials()[model->parts()[0].material].color,
                      (std::array<float, 3>{color, color, color}));
            expectClosed();
        }
        else
        {
            std::string reason = "opacity=1";
            if (value.name.starts_with("uv_"))
                reason = "UV transform";
            if (value.name == "multi_map")
                reason = "multiple base color";
            if (value.name == "bad_color")
                reason = "linear RGB";
            if (value.name == "texture_escape" || value.name == "texture_drive" || value.name == "texture_network")
                reason = "dependency";
            expectFailure(OBJ, reason);
        }
    }
    INSTANTIATE_TEST_SUITE_P(
        ObjMtl, ModelObjMaterialTest,
        ::testing::Values(MtlCase{"default", "", true}, MtlCase{"d_one", "d 1", true}, MtlCase{"tr_zero", "Tr 0", true},
                          MtlCase{"explicit_gray", "Kd 0.6 0.6 0.6", true}, MtlCase{"d_half", "d 0.5", false},
                          MtlCase{"tr_half", "Tr 0.5", false}, MtlCase{"d_high", "d 2", false},
                          MtlCase{"tr_negative", "Tr -1", false}, MtlCase{"map_d", "d 1\nmap_d alpha.png", false},
                          MtlCase{"d_then_tr_opaque", "d 0.5\nTr 0", true},
                          MtlCase{"tr_then_d_half", "Tr 0\nd 0.5", false},
                          MtlCase{"uv_offset", "map_Kd -o 0 0 0 ../../../textures/direction_rgba.png", false},
                          MtlCase{"uv_scale", "map_Kd -s 1 1 1 ../../../textures/direction_rgba.png", false},
                          MtlCase{"multi_map", "map_Kd a.png\nmap_Kd b.png", false},
                          MtlCase{"bad_color", "Kd 2 1 1", false},
                          MtlCase{"texture_escape", "map_Kd ../../../../../../outside.png", false},
                          MtlCase{"texture_drive", "map_Kd C:/outside.png", false},
                          MtlCase{"texture_network", "map_Kd https://example.invalid/a.png", false}),
        [](const auto& info) { return info.param.name; });

    TEST_F(ModelLoaderTest, RejectsMtlUvOptionsRegardlessOfCase)
    {
        for (const char* option : {"-O 0 0 0", "-S 2 2 1", "-T 1 1 1", "-TyPe sphere"})
        {
            SCOPED_TRACE(option);
            write("models/minimal_obj/materials/model.mtl",
                  std::string("newmtl Direction\nKd 1 1 1\nmap_Kd ") + option +
                      " ../../../textures/direction_rgba.png\nnewmtl Solid\nKd 0.5 1 0.5\n");
            expectFailure(OBJ, "UV transform/mapping");
        }
    }
    TEST_F(ModelLoaderTest, MissingRequiredMtlCannotFallBackToDefaultMaterialOrSiblingMtl)
    {
        const auto valid = read("models/minimal_obj/materials/model.mtl");
        fs::remove("assets/models/minimal_obj/materials/model.mtl");
        write("models/minimal_obj/model.mtl", valid);
        expectFailure(OBJ, "materials/model.mtl");
    }
    TEST_F(ModelLoaderTest, EscapingMtlPathFailsEvenIfOutsideFileExists)
    {
        auto text = read(OBJ);
        text.replace(text.find("materials/model.mtl"), std::strlen("materials/model.mtl"), "../../../outside.mtl");
        write(OBJ, text);
        std::ofstream("outside.mtl") << "newmtl Direction\nKd 1 1 1\n";
        expectFailure(OBJ, "escapes root");
    }
    TEST_F(ModelLoaderTest, UnsupportedFileExtensionAndBrokenObjFail)
    {
        write("models/unsupported.fbx", read(GLTF));
        expectFailure("models/unsupported.fbx", "Only static");
        write(OBJ, "v bad vertex\nf 99 100 101\n");
        expectFailure(OBJ, "ModelLoader");
    }

    TEST_F(ModelLoaderTest, ObjWithoutAnyMaterialUsesExplicitCpuWhiteDefault)
    {
        write(OBJ, "o Plain\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        const auto* model = load(OBJ);
        ASSERT_NE(model, nullptr) << messages_.str();
        EXPECT_FALSE(model->materials()[model->parts()[0].material].texturePath.has_value());
        EXPECT_EQ(model->materials()[model->parts()[0].material].color, (std::array<float, 3>{1, 1, 1}));
        expectClosed();
    }

    TEST_F(ModelLoaderTest, ObjMissingNamedMaterialFailsInsteadOfChoosingDefault)
    {
        write("models/minimal_obj/materials/model.mtl", "newmtl Other\nKd 1 1 1\n");
        expectFailure(OBJ, "failed to locate material");
    }

    TEST_F(ModelLoaderTest, TrailingWhitespaceInMtlNamePreservesExplicitColor)
    {
        write("models/minimal_obj/materials/model.mtl",
              "newmtl Direction   \r\nKd 0.6 0.6 0.6\r\nnewmtl Solid\r\nKd 1 1 1\r\n");
        const auto* model = load(OBJ);
        ASSERT_NE(model, nullptr) << messages_.str();
        EXPECT_EQ(model->materials()[model->parts()[0].material].color, (std::array<float, 3>{0.6f, 0.6f, 0.6f}));
        expectClosed();
    }
}  // namespace
