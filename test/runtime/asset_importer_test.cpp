#include "model_loader_fixture.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource;
    using namespace Incubator::Test;
    namespace fs = std::filesystem;

    TEST_F(AssetImporterTest, GltfCopiesNestedDependenciesAndLoadsThroughManager)
    {
        auto text = read(GLTF);
        const std::string originalUri = "../../textures/direction_rgba.png";
        const auto uri = text.find(originalUri);
        ASSERT_NE(uri, std::string::npos);
        text.replace(uri, originalUri.size(), "textures/nested/with%20space.png");
        writeExternal("External.GLTF", text);
        const auto mesh = read("models/minimal_gltf/geometry/mesh.bin");
        const auto texture = read("textures/direction_rgba.png");
        ASSERT_FALSE(mesh.empty());
        ASSERT_FALSE(texture.empty());
        writeExternal("geometry/mesh.bin", mesh);
        writeExternal("textures/nested/with space.png", texture);

        const auto imported = importExternalModel(*source_, external("External.GLTF"));
        EXPECT_EQ(imported.targetDirectory, "models/external");
        EXPECT_EQ(imported.virtualPath, "models/external/external.gltf");
        EXPECT_EQ(imported.copiedVirtualPaths,
                  (std::vector<std::string>{"models/external/external.gltf", "models/external/geometry/mesh.bin",
                                            "models/external/textures/nested/with space.png"}));
        EXPECT_EQ(read(imported.virtualPath), text);
        EXPECT_EQ(read("models/external/geometry/mesh.bin"), mesh);
        EXPECT_EQ(read("models/external/textures/nested/with space.png"), texture);
        expectModelTexture(imported, "models/external/textures/nested/with space.png");
    }

    TEST_F(AssetImporterTest, ObjResolvesDiffuseTextureRelativeToMtlAndLoadsThroughManager)
    {
        writeExternal("外部 模型.OBJ", read(OBJ));
        const std::string material = "newmtl Direction\nKd 1 1 1\nmap_Kd -clamp on ../textures/with space.png\n"
                                     "newmtl Solid\nKd 0.5 1 0.5\n";
        writeExternal("materials/model.mtl", material);
        const auto texture = read("textures/direction_rgba.png");
        ASSERT_FALSE(texture.empty());
        writeExternal("textures/with space.png", texture);

        const auto imported = importExternalModel(*source_, external("外部 模型.OBJ"), {""});
        EXPECT_EQ(imported.targetDirectory, "外部 模型");
        EXPECT_EQ(imported.virtualPath, "外部 模型/外部 模型.obj");
        EXPECT_EQ(imported.copiedVirtualPaths,
                  (std::vector<std::string>{"外部 模型/外部 模型.obj", "外部 模型/materials/model.mtl",
                                            "外部 模型/textures/with space.png"}));
        EXPECT_EQ(read("外部 模型/materials/model.mtl"), material);
        EXPECT_EQ(read("外部 模型/textures/with space.png"), texture);
        expectModelTexture(imported, "外部 模型/textures/with space.png");
    }

    TEST_F(AssetImporterTest, ImportsGltfAndObjWithExternalJpegTextures)
    {
        const auto jpeg = read("textures/reference_rgb.jpg");
        ASSERT_FALSE(jpeg.empty());
        auto gltf = read(GLTF);
        constexpr std::string_view oldUri = "../../textures/direction_rgba.png";
        const auto uri = gltf.find(oldUri);
        ASSERT_NE(uri, std::string::npos);
        gltf.replace(uri, oldUri.size(), "textures/reference.JPG");
        const auto pbr = gltf.find("\"pbrMetallicRoughness\"");
        ASSERT_NE(pbr, std::string::npos);
        gltf.insert(pbr, R"("normalTexture":{"index":0},)");
        writeExternal("gltf/model.gltf", gltf);
        writeExternal("gltf/geometry/mesh.bin", read("models/minimal_gltf/geometry/mesh.bin"));
        writeExternal("gltf/textures/reference.JPG", jpeg);
        writeExternal("obj/triangle.obj", read(OBJ));
        writeExternal("obj/materials/model.mtl",
                      "newmtl Direction\nmap_Kd ../textures/reference.JPEG\nnewmtl Solid\n");
        writeExternal("obj/textures/reference.JPEG", jpeg);

        const auto gltfImport = importExternalModel(*source_, external("gltf/model.gltf"));
        const auto objImport = importExternalModel(*source_, external("obj/triangle.obj"));
        ResourceManager::instance().registerLoader(ResourceType::TEXTURE, std::make_unique<TextureLoader>(*source_));
        for (const auto& [imported, texture] :
             {std::pair{&gltfImport, "models/model/textures/reference.jpg"},
              std::pair{&objImport, "models/triangle/textures/reference.jpeg"}})
        {
            SCOPED_TRACE(imported->virtualPath);
            EXPECT_EQ(read(texture), jpeg);
            expectModelTexture(*imported, texture);
            if (imported == &gltfImport)
            {
                const auto* model = load(imported->virtualPath);
                ASSERT_NE(model, nullptr);
                EXPECT_EQ(model->materials()[model->parts()[0].material].normalTexturePath, texture);
            }
            const auto hold = ResourceManager::instance().hold<TextureResource>(ResourceType::TEXTURE, texture);
            ASSERT_TRUE(hold);
            EXPECT_EQ(hold.get<TextureResource>()->width(), 3u);
        }
    }

    TEST_F(AssetImporterTest, GlbImportsOneFileAndLoadsEmbeddedPngAndJpeg)
    {
        const auto bytes = read("models/embedded_textures.glb");
        ASSERT_FALSE(bytes.empty());
        writeExternal("Embedded.GLB", bytes);
        const auto imported = importExternalModel(*source_, external("Embedded.GLB"));
        EXPECT_EQ(imported.virtualPath, "models/embedded/embedded.glb");
        EXPECT_EQ(imported.copiedVirtualPaths, std::vector<std::string>{imported.virtualPath});
        EXPECT_EQ(read(imported.virtualPath), bytes);
        const auto entries = source_->listDirectory(imported.targetDirectory);
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries.front().kind, AssetKind::Model);
        const auto* model = load(imported.virtualPath);
        ASSERT_NE(model, nullptr) << messages_.str();
        ASSERT_EQ(model->parts().size(), 2u);
        ASSERT_EQ(model->embeddedTextures().size(), 2u);
        const auto& material = model->materials()[model->parts().front().material];
        EXPECT_FALSE(material.texturePath);
        EXPECT_FALSE(material.normalTexturePath);
        EXPECT_FALSE(material.metallicRoughnessTexturePath);
        ASSERT_TRUE(material.embeddedTextureIndex);
        EXPECT_EQ(material.embeddedNormalTextureIndex, material.embeddedTextureIndex);
        EXPECT_EQ(material.embeddedEmissiveTextureIndex, material.embeddedTextureIndex);
        EXPECT_FALSE(material.emissiveTexturePath);
        ASSERT_TRUE(material.embeddedMetallicRoughnessTextureIndex);
        EXPECT_NE(material.embeddedMetallicRoughnessTextureIndex, material.embeddedTextureIndex);
        const auto& png = model->embeddedTextures()[*material.embeddedTextureIndex];
        EXPECT_EQ(png.width, 3u);
        EXPECT_EQ(png.height, 4u);
        const auto& jpeg = model->embeddedTextures()[*material.embeddedMetallicRoughnessTextureIndex];
        EXPECT_EQ(jpeg.width, 3u);
        EXPECT_EQ(jpeg.height, 2u);
        ASSERT_EQ(jpeg.rgba8.size(), 24u);
        EXPECT_NEAR(jpeg.rgba8.front(), 225, 2);
        EXPECT_EQ(jpeg.rgba8[3], 255u);
        expectClosed();
    }

    TEST_F(AssetImporterTest, GlbRejectsInvalidContainersAndDependenciesBeforeCopyOrLoad)
    {
        const auto fixture = read("models/embedded_textures.glb");
        ASSERT_GE(fixture.size(), 20u);
        const auto word = [](std::string_view bytes, size_t offset) {
            uint32_t value = 0;
            for (unsigned i = 0; i < 4; ++i)
                value |= uint32_t(static_cast<uint8_t>(bytes[offset + i])) << (8u * i);
            return value;
        };
        const auto writeWord = [](std::string& bytes, size_t offset, uint32_t value) {
            for (unsigned i = 0; i < 4; ++i)
                bytes[offset + i] = static_cast<char>((value >> (8u * i)) & 0xffu);
        };
        const auto verify = [&](std::string_view bytes, std::string_view reason) {
            SCOPED_TRACE(reason);
            writeExternal("rejected.glb", bytes);
            expectImportFailure("rejected.glb", reason);
            constexpr auto path = "models/rejected.glb";
            write(path, bytes);
            try
            {
                ModelLoader(*source_).load(ResourceId::fromPath(ResourceType::MODEL, path), path);
                ADD_FAILURE() << "Expected invalid GLB to fail loading";
            }
            catch (const Error::Exception& exception)
            {
                EXPECT_EQ(exception.code(), Error::Code::IO);
                EXPECT_NE(exception.message().find(reason), std::string::npos) << exception.what();
            }
            expectClosed();
        };
        auto bytes = fixture;
        bytes[0] = 'x';
        verify(bytes, "magic");
        bytes = fixture;
        writeWord(bytes, 4, 1);
        verify(bytes, "version 2");
        bytes = fixture;
        bytes.pop_back();
        verify(bytes, "declared length");
        writeWord(bytes, 8, static_cast<uint32_t>(bytes.size()));
        verify(bytes, "BIN chunk type or length");
        bytes = fixture;
        writeWord(bytes, 12, 3);
        verify(bytes, "JSON chunk type or length");

        const auto jsonLength = word(fixture, 12);
        ASSERT_LT(20u + jsonLength, fixture.size());
        bytes = fixture.substr(0, 20 + jsonLength);
        writeWord(bytes, 8, static_cast<uint32_t>(bytes.size()));
        verify(bytes, "lacks the BIN chunk");
        const auto changeJson = [&](std::string_view original, std::string_view replacement, std::string_view reason) {
            auto json = fixture.substr(20, jsonLength);
            const auto position = json.find(original);
            ASSERT_NE(position, std::string::npos);
            json.replace(position, original.size(), replacement);
            json.append((4 - json.size() % 4) % 4, ' ');
            auto changed = fixture.substr(0, 20) + json + fixture.substr(20 + jsonLength);
            writeWord(changed, 8, static_cast<uint32_t>(changed.size()));
            writeWord(changed, 12, static_cast<uint32_t>(json.size()));
            verify(changed, reason);
        };
        changeJson("Direction", std::string(1, static_cast<char>(0xff)), "Invalid glTF JSON");
        changeJson(R"("asset":)", R"("animations":[{}],"asset":)", "Unsupported glTF animations");
        changeJson(R"("asset":)", R"("skins":[{}],"asset":)", "Unsupported glTF skins");
        changeJson(R"("primitives":[{)", R"("primitives":[{"extensions":{"KHR_draco_mesh_compression":{}} ,)",
                   "compressed model geometry");
        changeJson(R"("buffers":[{)", R"("buffers":[{"uri":"data:application/octet-stream;base64,AA==",)",
                   "embedded buffer without URI");
        changeJson(R"("buffers":[{)", R"("buffers":[{"uri":"geometry/mesh.bin",)", "embedded buffer without URI");
        changeJson(R"("images":[{)", R"("images":[{"uri":"textures/texture.png",)", "embedded without URI");
        changeJson(R"("images":[{)", R"("images":[{"uri":"data:image/png;base64,AA==",)", "embedded without URI");
        changeJson("image/png", "image/webp", "mimeType");
        changeJson(R"("bufferView":5,"mimeType")", R"("bufferView":999,"mimeType")", "image bufferView index");
        changeJson(R"("byteOffset":136)", R"("byteOffset":999999)", "bufferView exceeds the BIN buffer");
    }

    TEST_F(AssetImporterTest, RejectsUnsupportedFormatMissingDependenciesAndInvalidPaths)
    {
        writeExternal("model.fbx", "{}");
        expectImportFailure("model.fbx", "Only external glTF, GLB and OBJ");
        writeExternal("model.obj", "mtllib missing.mtl\n");
        expectImportFailure("model.obj", "missing.mtl");
        writeExternal("model.obj", "mtllib material.mtl\n");
        writeExternal("material.mtl", "newmtl Surface\nmap_Kd textures/missing.png\n");
        expectImportFailure("model.obj", "textures");
        writeExternal("material.mtl", "newmtl Surface\nmap_Kd ../outside.png\n");
        expectImportFailure("model.obj", "escapes");
        writeExternal("model.gltf", R"({"buffers":[{"uri":"missing.bin"}]})");
        expectImportFailure("model.gltf", "missing.bin");
        writeExternal("model.gltf", R"({"images":[{"uri":"data:image/png;base64,AAAA"}]})");
        expectImportFailure("model.gltf", "data:image");
        writeExternal("model.gltf", R"({"buffers":[{"uri":"%2e%2e/outside.bin"}]})");
        expectImportFailure("model.gltf", "escapes");
        writeExternal("model.gltf", R"({"buffers":[{"uri":"/outside.bin"}]})");
        expectImportFailure("model.gltf", "outside.bin");
        writeExternal("model.gltf", R"({"buffers":[{"uri":"C:outside.bin"}]})");
        expectImportFailure("model.gltf", "C:");
    }

    TEST_F(AssetImporterTest, RejectsDirectoryReparsePoints)
    {
        writeExternal("model.gltf", R"({"buffers":[{"uri":"linked/mesh.bin"}]})");
        writeExternal("linked/mesh.bin", "mesh");
        fileIO_.reparseDirectory = "linked";
        expectImportFailure("model.gltf", "reparse point");
    }

    TEST_F(AssetImporterTest, RollsBackFilesAndDirectoriesOnCopyFailure)
    {
        writeExternal("model.gltf", R"({"buffers":[{"uri":"geometry/mesh.bin"}]})");
        writeExternal("geometry/mesh.bin", "mesh");
        fileIO_.counters.failPath = "mesh.bin";
        fileIO_.counters.throwRead = true;
        expectImportFailure("model.gltf", "controlled model read failure");
        EXPECT_TRUE(fs::exists(source_->resolve(GLTF)));
    }

    TEST_F(AssetImporterTest, RepeatedImportsAvoidExistingFilesAndDirectories)
    {
        writeExternal("model.obj", read(OBJ));
        writeExternal("materials/model.mtl", "newmtl Direction\nKd 1 1 1\nnewmtl Solid\nKd 0.5 1 0.5\n");
        write("models/model", "existing file");
        const auto first = importExternalModel(*source_, external("model.obj"));
        EXPECT_EQ(first.virtualPath, "models/model_1/model.obj");
        const auto firstModel = read(first.virtualPath);
        const auto firstMaterial = read("models/model_1/materials/model.mtl");
        writeExternal("model.obj", read(OBJ) + "\n# second import\n");
        writeExternal("materials/model.mtl", firstMaterial + "\n# second import\n");
        const auto second = importExternalModel(*source_, external("model.obj"));
        EXPECT_EQ(second.virtualPath, "models/model_2/model.obj");
        EXPECT_EQ(read("models/model"), "existing file");
        EXPECT_EQ(read(first.virtualPath), firstModel);
        EXPECT_EQ(read("models/model_1/materials/model.mtl"), firstMaterial);
        EXPECT_EQ(read(second.virtualPath), firstModel + "\n# second import\n");
    }
}  // namespace
