#include "runtime/resource/loader/model_loader.h"

#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <array>
#include <assimp/DefaultLogger.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/resource/asset_importer.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/resource/loader/model_resource.h"
#include "runtime/resource/loader/texture_loader.h"
#include "runtime/resource/loader/texture_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"

namespace
{
    namespace fs = std::filesystem;
    namespace Fio = Incubator::FileIO;
    using namespace Incubator;
    using namespace Incubator::Resource;
    constexpr const char* GLTF = "models/minimal_gltf/model.gltf";
    constexpr const char* OBJ = "models/minimal_obj/model.obj";

    struct IoCounters
    {
        size_t opened = 0;
        size_t destroyed = 0;
        size_t active = 0;
        size_t readCalls = 0;
        size_t seekCalls = 0;
        size_t chunk = std::numeric_limits<size_t>::max();
        std::string failPath;
        bool throwRead = false;
        bool earlyEof = false;
        std::vector<std::string> paths;
    };

    class TrackedHandle final : public Fio::FileHandle
    {
    public:
        TrackedHandle(std::unique_ptr<Fio::FileHandle> inner, IoCounters& counters, bool fail)
            : inner_(std::move(inner)), counters_(counters), fail_(fail)
        {
            ++counters_.opened;
            ++counters_.active;
        }
        ~TrackedHandle() override
        {
            --counters_.active;
            ++counters_.destroyed;
        }
        bool isValid() const override
        {
            return inner_->isValid();
        }
        size_t read(std::span<uint8_t> buffer) override
        {
            ++counters_.readCalls;
            if (fail_ && counters_.throwRead)
                throw Error::Exception(Error::Code::IO, "controlled model read failure");
            if (fail_ && counters_.earlyEof)
                return 0;
            return inner_->read(buffer.first(std::min(buffer.size(), counters_.chunk)));
        }
        size_t write(std::span<const uint8_t> buffer) override
        {
            return inner_->write(buffer);
        }
        size_t seek(int64_t offset, Fio::SeekOrigin origin) override
        {
            ++counters_.seekCalls;
            return inner_->seek(offset, origin);
        }
        uint64_t tell() const override
        {
            return inner_->tell();
        }
        uint64_t size() const override
        {
            return inner_->size();
        }
        void close() override
        {
            inner_->close();
        }
        void flush() override
        {
            inner_->flush();
        }

    private:
        std::unique_ptr<Fio::FileHandle> inner_;
        IoCounters& counters_;
        bool fail_;
    };

    class TrackedFileIO final : public Fio::FileIO
    {
    public:
        TrackedFileIO() : inner_(Fio::createPlatformFileIO()) {}
        IoCounters counters;
        std::string reparseDirectory;
        bool exists(const fs::path& path) override
        {
            return inner_->exists(path);
        }
        uint64_t getFileSize(const fs::path& path) override
        {
            return inner_->getFileSize(path);
        }
        bool remove(const fs::path& path) override
        {
            return inner_->remove(path);
        }
        std::unique_ptr<Fio::FileHandle> open(const fs::path& path, Fio::OpenMode mode, Fio::ShareMode share) override
        {
            const auto utf8 = Utils::Utf8::toUtf8String(path.lexically_normal().generic_wstring());
            counters.paths.push_back(utf8);
            auto file = inner_->open(path, mode, share);
            if (!file)
                return nullptr;
            return std::make_unique<TrackedHandle>(
                std::move(file), counters,
                !counters.failPath.empty() && utf8.find(counters.failPath) != std::string::npos);
        }
        std::vector<Fio::DirectoryEntry> listDirectory(const std::filesystem::path& path) override
        {
            return inner_->listDirectory(path);
        }
        std::optional<Fio::DirectoryEntry> inspect(const fs::path& path) override
        {
            auto entry = inner_->inspect(path);
            if (entry && entry->directory && entry->name == reparseDirectory)
                entry->reparsePoint = true;
            return entry;
        }
        bool directoryExists(const fs::path& path) override
        {
            return inner_->directoryExists(path);
        }
        bool createDirectory(const fs::path& path) override
        {
            return inner_->createDirectory(path);
        }
        bool removeDirectory(const fs::path& path) override
        {
            return inner_->removeDirectory(path);
        }

    private:
        std::unique_ptr<Fio::FileIO> inner_;
    };

    class ModelLoaderTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            previous_ = fs::current_path();
            temporary_ =
                fs::temp_directory_path() / "IncubatorModelLoader" / std::to_string(reinterpret_cast<uintptr_t>(this));
            fs::remove_all(temporary_);
            fs::create_directories(temporary_ / "assets");
            fs::copy(Utils::Utf8::toFsPath(MODEL_BUILD_ASSET_DIR) / "models", temporary_ / "assets/models",
                     fs::copy_options::recursive);
            fs::copy(Utils::Utf8::toFsPath(MODEL_BUILD_ASSET_DIR) / "textures", temporary_ / "assets/textures",
                     fs::copy_options::recursive);
            fs::current_path(temporary_);
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            Log::Registry::reset();
            Log::Registry::get("resource").addSink(std::make_shared<spdlog::sinks::ostream_sink_mt>(messages_));
            source_ = std::make_unique<AssetSource>(fileIO_);
            ResourceManager::instance().registerLoader(ResourceType::MODEL, std::make_unique<ModelLoader>(*source_));
        }
        void copyDemoModel(std::string_view directory)
        {
            const auto source = Utils::Utf8::toFsPath(MODEL_DEMO_ASSET_DIR) / "models" / std::string(directory);
            const auto destination = temporary_ / "assets/models" / std::string(directory);
            fs::copy(source, destination, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        }
        void TearDown() override
        {
            holds_.clear();
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            Log::Registry::reset();
            fs::current_path(previous_);
            std::error_code error;
            fs::remove_all(temporary_, error);
        }
        std::string read(std::string_view path)
        {
            std::ifstream file(Utils::Utf8::toFsPath(std::string("assets/") + std::string(path)), std::ios::binary);
            return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        }
        void write(std::string_view path, std::string_view content)
        {
            const auto filePath = Utils::Utf8::toFsPath(std::string("assets/") + std::string(path));
            fs::create_directories(filePath.parent_path());
            std::ofstream file(filePath, std::ios::binary);
            file.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        void editGltf(const std::function<void(rapidjson::Document&)>& edit, std::string_view path = GLTF)
        {
            rapidjson::Document source;
            const auto text = read(path);
            source.Parse(text.c_str());
            ASSERT_FALSE(source.HasParseError());
            edit(source);
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            source.Accept(writer);
            write(path, buffer.GetString());
        }
        const ModelResource* load(std::string_view path)
        {
            holds_.push_back(ResourceManager::instance().hold<ModelResource>(ResourceType::MODEL, path));
            id_ = ResourceRegistry::instance().lookup(ResourceType::MODEL, path);
            return holds_.back().get<ModelResource>();
        }
        void expectClosed()
        {
            EXPECT_EQ(fileIO_.counters.active, 0u);
            EXPECT_EQ(fileIO_.counters.opened, fileIO_.counters.destroyed);
        }
        void expectFailure(std::string_view path, std::string_view reason)
        {
            EXPECT_EQ(load(path), nullptr);
            EXPECT_EQ(ResourceRegistry::instance().getObject(id_), nullptr);
            EXPECT_EQ(ResourceRegistry::instance().refCount(id_), 0u);
            EXPECT_NE(messages_.str().find(reason), std::string::npos) << messages_.str();
            EXPECT_NE(messages_.str().find(path), std::string::npos) << messages_.str();
            expectClosed();
        }
        const ModelNode& node(const ModelResource& model, const char* name)
        {
            const auto found = std::find_if(model.nodes().begin(), model.nodes().end(),
                                            [name](const auto& value) { return value.name == name; });
            EXPECT_NE(found, model.nodes().end());
            return *found;
        }
        fs::path previous_;
        fs::path temporary_;
        TrackedFileIO fileIO_;
        std::unique_ptr<AssetSource> source_;
        ResourceId id_;
        std::vector<CpuResource> holds_;
        std::ostringstream messages_;
    };

    class AssetImporterTest : public ModelLoaderTest
    {
    protected:
        void writeExternal(std::string_view name, std::string_view bytes)
        {
            write(std::string("../external/") + std::string(name), bytes);
        }

        fs::path external(std::string_view name)
        {
            return temporary_ / "external" / Utils::Utf8::toFsPath(name);
        }

        void expectImportFailure(std::string_view name, std::string_view reason)
        {
            try
            {
                importExternalModel(*source_, external(name), {"new/imports"});
                ADD_FAILURE() << "Expected import to fail for " << name;
            }
            catch (const Error::Exception& error)
            {
                EXPECT_EQ(error.code(), Error::Code::IO);
                EXPECT_NE(error.message().find(reason), std::string::npos) << error.what();
            }
            EXPECT_FALSE(fs::exists(source_->root() / "new"));
            expectClosed();
        }

        void expectModelTexture(const ImportedModelResult& imported, std::string_view texturePath)
        {
            const auto* model = load(imported.virtualPath);
            ASSERT_NE(model, nullptr) << messages_.str();
            ASSERT_EQ(model->parts().size(), 2u);
            EXPECT_EQ(model->materials()[model->parts()[0].material].texturePath, texturePath);
            expectClosed();
        }
    };

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

    TEST_F(ModelLoaderTest, GltfThroughManagerPreservesHierarchyPartsAndSharedMeshes)
    {
        const auto* model = load(GLTF);
        ASSERT_NE(model, nullptr) << messages_.str();
        EXPECT_EQ(model->type(), ResourceType::MODEL);
        EXPECT_EQ(model->geometries().size(), 2u);
        EXPECT_EQ(model->parts().size(), 2u);
        EXPECT_EQ(model->nodes().size(), 4u);
        EXPECT_EQ(node(*model, "Leaf").parts, node(*model, "Shared").parts);
        EXPECT_EQ(node(*model, "Leaf").parts, (std::vector<uint32_t>{0, 1}));
        EXPECT_NE(model->parts()[0].material, model->parts()[1].material);
        EXPECT_EQ(*model->materials()[model->parts()[0].material].texturePath, "textures/direction_rgba.png");
        EXPECT_FALSE(model->materials()[model->parts()[1].material].texturePath.has_value());
        EXPECT_EQ(model->materials()[model->parts()[1].material].color, (std::array<float, 3>{0.5f, 1.0f, 0.5f}));
        EXPECT_FLOAT_EQ(model->geometries()[0].vertices[0].position[0], -1.0f);
        EXPECT_EQ(model->geometries()[0].indices, (std::vector<uint32_t>{0, 1, 2, 4, 5, 3}));
        expectClosed();
        ResourceManager::instance().reset();
        fs::remove_all("assets/models");
        EXPECT_EQ(node(*model, "SourceRoot").name, "SourceRoot");
        EXPECT_FLOAT_EQ(model->geometries()[1].vertices[2].position[1], 0.5f);
    }

    TEST_F(ModelLoaderTest, SourceNormalsAndUnlitSurviveGltfAndObjImport)
    {
        constexpr const char* sourcePath = "models/normals/folded.gltf";
        editGltf(
            [](auto& source) {
                source["meshes"][0]["primitives"][0]["attributes"].AddMember("NORMAL", 3, source.GetAllocator());
            },
            sourcePath);
        write("models/normals/required.gltf", read(sourcePath));
        editGltf(
            [](auto& source) {
                auto& a = source.GetAllocator();
                rapidjson::Value required(rapidjson::kArrayType);
                required.PushBack("KHR_materials_unlit", a);
                source.AddMember("extensionsRequired", required, a);
            },
            "models/normals/required.gltf");
        for (const char* path : {sourcePath, "models/normals/required.gltf", "models/normals/source.obj"})
        {
            SCOPED_TRACE(path);
            const auto* model = load(path);
            ASSERT_NE(model, nullptr) << messages_.str();
            ASSERT_EQ(model->geometries().size(), 1u);
            const auto& geometry = model->geometries()[0];
            const bool obj = std::string_view(path).ends_with(".obj");
            ASSERT_EQ(geometry.vertices.size(), obj ? 3u : 4u);
            for (size_t i = 0; i < geometry.vertices.size(); ++i)
            {
                const auto& normal = geometry.vertices[i].normal;
                EXPECT_EQ(normal, (i == geometry.vertices.size() - 1 ? std::array<float, 3>{}
                                                                     : std::array<float, 3>{0, -1, 0}));
            }
            size_t unlit = 0;
            for (const auto& material : model->materials())
                unlit += material.unlit;
            EXPECT_EQ(unlit, obj ? 0u : 1u);
            EXPECT_EQ(model->materials()[model->parts()[0].material].unlit, !obj);
            expectClosed();
        }
        auto bytes = read("models/normals/folded.bin");
        const auto invalid = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(bytes.data() + 92, &invalid, sizeof(invalid));
        write("models/normals/folded.bin", bytes);
        write("models/normals/nonfinite.gltf", read(sourcePath));
        expectFailure("models/normals/nonfinite.gltf", "ModelLoader");
    }

    TEST_F(ModelLoaderTest, MissingNormalsGenerateFlatGltfAndEightyDegreeSmoothObj)
    {
        const auto* gltf = load("models/normals/folded.gltf");
        ASSERT_NE(gltf, nullptr) << messages_.str();
        ASSERT_EQ(gltf->geometries().size(), 1u);
        const auto& folded = gltf->geometries()[0];
        ASSERT_EQ(folded.vertices.size(), 6u);
        ASSERT_EQ(folded.indices.size(), 6u);
        const std::array<std::array<float, 3>, 4> positions{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
        const std::array<std::array<float, 2>, 4> uvs{{{0, 0}, {1, 0}, {0, 1}, {1, 1}}};
        const std::array<size_t, 6> corners{0, 1, 2, 0, 3, 1};
        for (size_t i = 0; i < corners.size(); ++i)
        {
            const auto& vertex = folded.vertices[folded.indices[i]];
            EXPECT_EQ(vertex.position, positions[corners[i]]);
            EXPECT_EQ(vertex.uv, uvs[corners[i]]);
            EXPECT_EQ(vertex.normal, (i < 3 ? std::array<float, 3>{0, 0, 1} : std::array<float, 3>{0, 1, 0}));
        }
        EXPECT_EQ(gltf->geometryBounds()[0].min, glm::vec3(0));
        EXPECT_EQ(gltf->geometryBounds()[0].max, glm::vec3(1));
        EXPECT_EQ(gltf->parts()[0].material, 0u);
        const auto* obj = load("models/normals/folded.obj");
        ASSERT_NE(obj, nullptr) << messages_.str();
        ASSERT_EQ(obj->geometries().size(), 3u);
        for (size_t part = 0; part < 3; ++part)
        {
            const auto& geometry = obj->geometries()[part];
            ASSERT_EQ(geometry.indices.size(), part == 2 ? 3u : 6u);
            for (size_t corner = 0; corner < geometry.indices.size(); ++corner)
            {
                const auto& vertex = geometry.vertices[geometry.indices[corner]];
                std::array<float, 3> expected{};
                if (part == 0)
                    expected = corner % 3 < 2 ? std::array<float, 3>{0, -0.5f, std::sqrt(0.75f)}
                                              : (corner < 3 ? std::array<float, 3>{0, 0, 1}
                                                            : std::array<float, 3>{0, -std::sqrt(0.75f), 0.5f});
                else if (part == 1)
                    expected = corner < 3 ? std::array<float, 3>{0, 0, 1}
                                          : std::array<float, 3>{0, -0.98480775f, -0.17364818f};
                for (size_t axis = 0; axis < 3; ++axis)
                    EXPECT_NEAR(vertex.normal[axis], expected[axis], 0.00001f) << part << ':' << corner;
            }
        }
        for (const auto& material : obj->materials())
            EXPECT_FALSE(material.unlit);
        expectClosed();
    }

    TEST_F(ModelLoaderTest, GltfMatrixConversionPreservesNonIdentityRootAndLocalProducts)
    {
        const auto* model = load(GLTF);
        ASSERT_NE(model, nullptr) << messages_.str();
        const auto& root = node(*model, "SourceRoot").localTransform;
        const auto& parent = node(*model, "Parent").localTransform;
        const auto& leaf = node(*model, "Leaf").localTransform;
        EXPECT_FLOAT_EQ(root[3][0], 0.25f);
        EXPECT_NEAR(parent[0][1], std::sqrt(0.5f), 0.00001f);
        EXPECT_NEAR(parent[1][0], -std::sqrt(0.5f), 0.00001f);
        EXPECT_FLOAT_EQ(leaf[3][0], 0.1f);
        EXPECT_FLOAT_EQ(leaf[0][0], 0.75f);
        const auto point = root * parent * leaf * glm::vec4(1, 1, 0, 1);
        EXPECT_NEAR(point.x, 0.25f + 0.15f * std::sqrt(0.5f), 0.00001f);
        EXPECT_NEAR(point.y, 1.55f * std::sqrt(0.5f), 0.00001f);
        EXPECT_FLOAT_EQ(point.w, 1.0f);
        expectClosed();
    }

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

    TEST_F(ModelLoaderTest, ImportsFixedShowcaseModelsThroughResourceManager)
    {
        constexpr std::string_view teapotPath = "models/utah_teapot/utah_teapot.obj";
        constexpr std::string_view avocadoPath = "models/avocado/glTF/Avocado.gltf";
        copyDemoModel("utah_teapot");
        copyDemoModel("avocado");
        const auto verifyGeometry = [](const ModelResource& model) {
            ASSERT_FALSE(model.geometries().empty());
            ASSERT_FALSE(model.parts().empty());
            ASSERT_FALSE(model.nodes().empty());
            for (const auto& geometry : model.geometries())
            {
                EXPECT_FALSE(geometry.vertices.empty());
                EXPECT_FALSE(geometry.indices.empty());
                EXPECT_EQ(geometry.indices.size() % 3, 0u);
                for (const auto index : geometry.indices)
                    EXPECT_LT(index, geometry.vertices.size());
                size_t usableNormals = 0;
                for (const auto& vertex : geometry.vertices)
                {
                    const auto& n = vertex.normal;
                    const auto length = std::hypot(n[0], n[1], n[2]);
                    EXPECT_TRUE(std::isfinite(length));
                    if (length > 0)
                    {
                        ++usableNormals;
                        EXPECT_NEAR(length, 1.0f, 0.00001f);
                    }
                }
                EXPECT_GT(usableNormals, 0u);
            }
        };

        const auto* teapot = load(teapotPath);
        ASSERT_NE(teapot, nullptr) << messages_.str();
        verifyGeometry(*teapot);
        EXPECT_TRUE(std::none_of(teapot->materials().begin(), teapot->materials().end(),
                                 [](const auto& material) { return material.texturePath.has_value(); }));
        expectClosed();

        const auto* avocado = load(avocadoPath);
        ASSERT_NE(avocado, nullptr) << messages_.str();
        verifyGeometry(*avocado);
        ASSERT_TRUE(std::any_of(avocado->materials().begin(), avocado->materials().end(),
                                [](const auto& material) { return material.texturePath.has_value(); }));
        const auto baseColor = std::find_if(avocado->materials().begin(), avocado->materials().end(),
                                            [](const auto& material) { return material.texturePath.has_value(); });
        EXPECT_EQ(*baseColor->texturePath, "models/avocado/gltf/avocado_basecolor.png");
        EXPECT_TRUE(fs::exists("assets/models/avocado/glTF/Avocado_roughnessMetallic.png"));
        EXPECT_TRUE(fs::exists("assets/models/avocado/glTF/Avocado_normal.png"));
        EXPECT_TRUE(std::any_of(avocado->geometries().begin(), avocado->geometries().end(), [](const auto& geometry) {
            return std::any_of(geometry.vertices.begin(), geometry.vertices.end(),
                               [](const auto& vertex) { return vertex.uv[0] != 0 || vertex.uv[1] != 0; });
        }));
        expectClosed();
    }

    TEST_F(ModelLoaderTest, BothFormatUvsPointAtSameDirectionalPngRowsWithoutVertexBake)
    {
        const auto* gltf = load(GLTF);
        ASSERT_NE(gltf, nullptr) << messages_.str();
        const auto* obj = load(OBJ);
        ASSERT_NE(obj, nullptr) << messages_.str();
        const std::array<std::array<float, 2>, 4> expected = {{{0, 0}, {0, 1}, {1, 1}, {1, 0}}};
        for (size_t i = 0; i < 4; ++i)
        {
            EXPECT_EQ(gltf->geometries()[0].vertices[i].uv, expected[i]);
            EXPECT_EQ(obj->geometries()[0].vertices[i].uv, expected[i]);
            EXPECT_EQ(gltf->geometries()[0].vertices[i].position, obj->geometries()[0].vertices[i].position);
        }
        ResourceManager::instance().registerLoader(ResourceType::TEXTURE, std::make_unique<TextureLoader>(*source_));
        const auto textureHold =
            ResourceManager::instance().hold<TextureResource>(ResourceType::TEXTURE, "textures/direction_rgba.png");
        const auto* texture = textureHold.get<TextureResource>();
        ASSERT_NE(texture, nullptr);
        EXPECT_GT(texture->width(), 1u);
        EXPECT_GT(texture->height(), 1u);
        // 顶部行和底部行使用不同数据；两种格式中网格上方顶点的 V 坐标均为 0。
        const auto stride = static_cast<size_t>(texture->width()) * 4;
        EXPECT_FALSE(std::equal(texture->pixels().begin(), texture->pixels().begin() + stride,
                                texture->pixels().begin() + stride * (texture->height() - 1)));
        expectClosed();
    }

    TEST_F(ModelLoaderTest, ShortChunksAreAccumulatedAndAllNativeHandlesAreReleased)
    {
        fileIO_.counters.chunk = 7;
        ASSERT_NE(load(GLTF), nullptr) << messages_.str();
        ASSERT_NE(load(OBJ), nullptr) << messages_.str();
        EXPECT_GT(fileIO_.counters.readCalls, 100u);
        EXPECT_GT(fileIO_.counters.seekCalls, 0u);
        expectClosed();
    }

    TEST_F(ModelLoaderTest, MissingPngIsRecordedWithoutRecursiveImageLoad)
    {
        fs::remove("assets/textures/direction_rgba.png");
        ASSERT_NE(load(GLTF), nullptr) << messages_.str();
        ASSERT_NE(load(OBJ), nullptr) << messages_.str();
        for (const auto& path : fileIO_.counters.paths)
            EXPECT_EQ(path.find(".png"), std::string::npos);
        expectClosed();
    }

    TEST_F(ModelLoaderTest, ChineseEmojiModelBinAndMtlPathsResolveWithinAssetRoot)
    {
        fs::rename("assets/models/minimal_gltf", Utils::Utf8::toFsPath("assets/models/模型😀"));
        fs::rename(Utils::Utf8::toFsPath("assets/models/模型😀/geometry/mesh.bin"),
                   Utils::Utf8::toFsPath("assets/models/模型😀/geometry/数据 😀.bin"));
        auto text = read("models/模型😀/model.gltf");
        const auto at = text.find("geometry/mesh.bin");
        ASSERT_NE(at, std::string::npos);
        text.replace(at, std::strlen("geometry/mesh.bin"), "geometry/%E6%95%B0%E6%8D%AE%20%F0%9F%98%80.bin");
        text.replace(text.find("direction_rgba.png"), std::strlen("direction_rgba.png"), "方向%20%F0%9F%98%80.png");
        write("models/模型😀/model.gltf", text);
        fs::rename("assets/textures/direction_rgba.png", Utils::Utf8::toFsPath("assets/textures/方向 😀.png"));
        const auto* gltf = load("models/模型😀/model.gltf");
        ASSERT_NE(gltf, nullptr) << messages_.str();
        EXPECT_EQ(*gltf->materials()[gltf->parts()[0].material].texturePath, "textures/方向 😀.png");
        fs::rename("assets/models/minimal_obj/materials/model.mtl",
                   Utils::Utf8::toFsPath("assets/models/minimal_obj/materials/材质😀.mtl"));
        text = read(OBJ);
        text.replace(text.find("materials/model.mtl"), std::strlen("materials/model.mtl"), "materials/材质😀.mtl");
        write(OBJ, text);
        text = read("models/minimal_obj/materials/材质😀.mtl");
        text.replace(text.find("direction_rgba.png"), std::strlen("direction_rgba.png"), "方向 😀.png");
        write("models/minimal_obj/materials/材质😀.mtl", text);
        const auto* obj = load(OBJ);
        ASSERT_NE(obj, nullptr) << messages_.str();
        EXPECT_EQ(*obj->materials()[obj->parts()[0].material].texturePath, "textures/方向 😀.png");
        expectClosed();
    }

    TEST_F(ModelLoaderTest, DirectFailureDoesNotLogAndManagerLogsOnceWithoutPartialMount)
    {
        ModelLoader direct(*source_);
        const auto id = ResourceId::fromPath(ResourceType::MODEL, "模型😀/missing.gltf");
        EXPECT_THROW(direct.load(id, "模型😀/missing.gltf"), Error::Exception);
        EXPECT_TRUE(messages_.str().empty());
        expectFailure("模型😀/missing.gltf", "ModelLoader");
        const auto message = messages_.str();
        const auto first = message.find("load: loader threw:");
        ASSERT_NE(first, std::string::npos);
        EXPECT_EQ(message.find("load: loader threw:", first + 1), std::string::npos);
    }

    TEST_F(ModelLoaderTest, CorrectingBadFileAllowsRetryWithoutNewRegistration)
    {
        const auto valid = read(GLTF);
        write(GLTF, "broken JSON");
        expectFailure(GLTF, "Invalid glTF JSON");
        const auto failedId = id_;
        write(GLTF, valid);
        const auto* retried = load(GLTF);
        ASSERT_NE(retried, nullptr);
        EXPECT_EQ(retried->id(), failedId);
        expectClosed();
    }
    class ModelGltfFailureTest : public ModelLoaderTest, public ::testing::WithParamInterface<std::string>
    {};
    TEST_P(ModelGltfFailureTest, RejectsUnsupportedSourceWithoutMounting)
    {
        const auto name = GetParam();
        std::string reason;
        editGltf([&](auto& source) {
            auto& a = source.GetAllocator();
            auto& material = source["materials"][0];
            auto& pbr = material["pbrMetallicRoughness"];
            auto& texture = pbr["baseColorTexture"];
            auto& primitive = source["meshes"][0]["primitives"][0];
            if (name == "uv1" || name == "negative_uv")
            {
                texture.AddMember("texCoord", name == "uv1" ? 1 : -1, a);
                primitive["attributes"].AddMember("TEXCOORD_1", 1, a);
                reason = "UV0";
            }
            else if (name == "missing_uv0")
            {
                primitive["attributes"].RemoveMember("TEXCOORD_0");
                reason = "UV0";
            }
            else if (name == "uv_transform" || name == "identity_uv_transform" || name == "transform_uv_override")
            {
                rapidjson::Value extensions(rapidjson::kObjectType), transform(rapidjson::kObjectType);
                if (name == "uv_transform")
                {
                    rapidjson::Value offset(rapidjson::kArrayType);
                    offset.PushBack(0.5, a).PushBack(0.0, a);
                    transform.AddMember("offset", offset, a);
                }
                if (name == "transform_uv_override")
                    transform.AddMember("texCoord", 1, a);
                extensions.AddMember("KHR_texture_transform", transform, a);
                texture.AddMember("extensions", extensions, a);
                reason = "UV transform";
            }
            else if (name == "mask" || name == "blend" || name == "unknown_alpha")
            {
                material["alphaMode"].SetString(name == "mask" ? "MASK" : name == "blend" ? "BLEND" : "INVALID", a);
                reason = "alphaMode OPAQUE";
            }
            else if (name == "negative_color" || name == "high_color")
            {
                pbr["baseColorFactor"][0].SetFloat(name == "negative_color" ? -0.1f : 1.1f);
                reason = "linear RGB";
            }
            else if (name == "root_escape" || name == "absolute_uri" || name == "network_uri" ||
                     name == "encoded_escape")
            {
                const char* uri = name == "root_escape"    ? "../../../outside.bin"
                                  : name == "absolute_uri" ? "C:/outside.bin"
                                  : name == "network_uri"  ? "https://example.invalid/mesh.bin"
                                                           : "%2E%2E/%2E%2E/%2E%2E/outside.bin";
                source["buffers"][0]["uri"].SetString(uri, a);
                reason = name == "root_escape" || name == "encoded_escape" ? "escapes assets root"
                                                                           : "Unsupported model dependency";
            }
            else if (name == "bad_texture_uri")
            {
                source["images"][0]["uri"].SetString("//server/image.png", a);
                reason = "Unsupported model dependency";
            }
            else if (name == "embedded_image" || name == "embedded_buffer")
            {
                source[name == "embedded_image" ? "images" : "buffers"][0]["uri"].SetString(
                    "data:application/octet-stream;base64,AAAA", a);
                reason = "Unsupported model dependency";
            }
            else if (name == "unsupported_image_format")
            {
                source["images"][0]["uri"].SetString("../../textures/image.webp", a);
                reason = "external PNG or JPEG";
            }
            else if (name == "animation" || name == "skin")
            {
                rapidjson::Value items(rapidjson::kArrayType);
                items.PushBack(rapidjson::Value(rapidjson::kObjectType), a);
                source.AddMember(rapidjson::Value(name == "animation" ? "animations" : "skins", a), items, a);
                reason = name == "animation" ? "animations" : "skins";
            }
            else if (name == "morph")
            {
                rapidjson::Value targets(rapidjson::kArrayType);
                targets.PushBack(rapidjson::Value(rapidjson::kObjectType), a);
                primitive.AddMember("targets", targets, a);
                reason = "Morph/compressed";
            }
            else if (name == "draco" || name == "meshopt")
            {
                rapidjson::Value extensions(rapidjson::kObjectType);
                extensions.AddMember(
                    rapidjson::Value(name == "draco" ? "KHR_draco_mesh_compression" : "EXT_meshopt_compression", a),
                    rapidjson::Value(rapidjson::kObjectType), a);
                if (name == "draco")
                    primitive.AddMember("extensions", extensions, a);
                else
                    source["bufferViews"][0].AddMember("extensions", extensions, a);
                reason = name == "draco" ? "Morph/compressed" : "Compressed model geometry";
            }
            else if (name == "version1")
            {
                source["asset"]["version"].SetString("1.0", a);
                reason = "glTF 2.0";
            }
            else if (name == "point_primitive")
            {
                primitive.AddMember("mode", 0, a);
                reason = "triangle list";
            }
            else if (name == "cycle" || name == "multiple_parents")
            {
                rapidjson::Value children(rapidjson::kArrayType);
                children.PushBack(name == "cycle" ? 0 : 2, a);
                source["nodes"][3].AddMember("children", children, a);
                reason = "hierarchy";
            }
            else if (name == "bad_node")
            {
                source["nodes"][0]["children"][0].SetUint(99);
                reason = "ModelLoader";
            }
        });
        ASSERT_FALSE(reason.empty());
        expectFailure(GLTF, reason);
    }
    INSTANTIATE_TEST_SUITE_P(UnsupportedGltf, ModelGltfFailureTest,
                             ::testing::Values("uv1", "negative_uv", "missing_uv0", "uv_transform",
                                               "identity_uv_transform", "transform_uv_override", "mask", "blend",
                                               "unknown_alpha", "negative_color", "high_color", "root_escape",
                                               "absolute_uri", "network_uri", "encoded_escape", "bad_texture_uri",
                                               "embedded_image", "embedded_buffer", "unsupported_image_format",
                                               "animation", "skin",
                                               "morph", "draco", "meshopt", "version1", "point_primitive", "cycle",
                                               "multiple_parents", "bad_node"),
                             [](const auto& info) { return info.param; });

    class ModelGltfAcceptedTest : public ModelLoaderTest, public ::testing::WithParamInterface<std::string>
    {};
    TEST_P(ModelGltfAcceptedTest, AcceptsOpaqueDefaultsAndPureColorWithoutUv)
    {
        const auto name = GetParam();
        editGltf([&](auto& source) {
            auto& mat = source["materials"][0];
            if (name == "default_alpha")
                mat.RemoveMember("alphaMode");
            if (name == "opaque_low_alpha")
                mat["pbrMetallicRoughness"]["baseColorFactor"][3].SetFloat(0.2f);
            if (name == "pure_color_no_uv" || name == "default_material")
            {
                mat["pbrMetallicRoughness"].RemoveMember("baseColorTexture");
                source["meshes"][0]["primitives"][0]["attributes"].RemoveMember("TEXCOORD_0");
                if (name == "default_material")
                    source["meshes"][0]["primitives"][0].RemoveMember("material");
            }
        });
        const auto* model = load(GLTF);
        ASSERT_NE(model, nullptr) << messages_.str();
        const auto& material = model->materials()[model->parts()[0].material];
        EXPECT_EQ(material.color, (std::array<float, 3>{1, 1, 1}));
        if (name == "pure_color_no_uv" || name == "default_material")
        {
            EXPECT_FALSE(material.texturePath.has_value());
            EXPECT_EQ(model->geometries()[0].vertices[0].uv, (std::array<float, 2>{0, 0}));
        }
        expectClosed();
    }
    INSTANTIATE_TEST_SUITE_P(SupportedGltf, ModelGltfAcceptedTest,
                             ::testing::Values("default_alpha", "opaque_low_alpha", "pure_color_no_uv",
                                               "default_material"),
                             [](const auto& info) { return info.param; });
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

    class ModelReadFailureTest : public ModelLoaderTest, public ::testing::WithParamInterface<std::string>
    {};
    TEST_P(ModelReadFailureTest, PropagatesIncompleteReadOrExceptionAndReleasesHandles)
    {
        const auto name = GetParam();
        fileIO_.counters.failPath = name.starts_with("gltf")  ? "model.gltf"
                                    : name.starts_with("bin") ? "mesh.bin"
                                                              : "model.mtl";
        fileIO_.counters.earlyEof = name.ends_with("eof");
        fileIO_.counters.throwRead = !fileIO_.counters.earlyEof;
        expectFailure(name.starts_with("mtl") ? OBJ : GLTF,
                      fileIO_.counters.earlyEof ? "Incomplete model read" : "controlled model read failure");
    }
    INSTANTIATE_TEST_SUITE_P(FileIoFailures, ModelReadFailureTest,
                             ::testing::Values("gltf_eof", "bin_exception", "mtl_eof"),
                             [](const auto& info) { return info.param; });

    TEST_F(ModelLoaderTest, MissingRequiredBinFailsWithDependencyContext)
    {
        fs::remove("assets/models/minimal_gltf/geometry/mesh.bin");
        expectFailure(GLTF, "mesh.bin");
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
    TEST_F(ModelLoaderTest, LoadsGltfGeometryBeyondUint16RangeWithHighIndex)
    {
        constexpr uint32_t vertexCount = 65537;
        constexpr size_t indexCount = 65538;
        const size_t positionBytes = static_cast<size_t>(vertexCount) * 3 * sizeof(float);
        const size_t indexBytes = indexCount * sizeof(uint32_t);
        std::string bytes(positionBytes + indexBytes, '\0');
        for (uint32_t vertex = 0; vertex < vertexCount; ++vertex)
        {
            const std::array<float, 3> position = {static_cast<float>(vertex % 257), static_cast<float>(vertex / 257),
                                                   0.0f};
            std::memcpy(bytes.data() + static_cast<size_t>(vertex) * sizeof(position), position.data(),
                        sizeof(position));
        }
        std::vector<uint32_t> indices;
        indices.reserve(indexCount);
        for (uint32_t index = 0; index < vertexCount; ++index)
            indices.push_back(index);
        indices.push_back(0);
        std::memcpy(bytes.data() + positionBytes, indices.data(), indexBytes);
        write("models/minimal_gltf/geometry/wide.bin", bytes);
        editGltf([&](auto& source) {
            auto& a = source.GetAllocator();
            source["buffers"][0]["uri"].SetString("geometry/wide.bin", a);
            source["buffers"][0]["byteLength"].SetUint64(bytes.size());
            source["bufferViews"][0]["byteLength"].SetUint(static_cast<uint32_t>(positionBytes));
            source["bufferViews"][2]["byteOffset"].SetUint(static_cast<uint32_t>(positionBytes));
            source["bufferViews"][2]["byteLength"].SetUint(static_cast<uint32_t>(indexBytes));
            source["accessors"][0]["count"].SetUint(vertexCount);
            rapidjson::Value minimum(rapidjson::kArrayType);
            minimum.PushBack(0.0, a).PushBack(0.0, a).PushBack(0.0, a);
            source["accessors"][0]["min"] = std::move(minimum);
            rapidjson::Value maximum(rapidjson::kArrayType);
            maximum.PushBack(256.0, a).PushBack(255.0, a).PushBack(0.0, a);
            source["accessors"][0]["max"] = std::move(maximum);
            source["accessors"][2]["componentType"].SetUint(5125);
            source["accessors"][2]["count"].SetUint(static_cast<uint32_t>(indexCount));
            source["meshes"][0]["primitives"].Erase(source["meshes"][0]["primitives"].Begin() + 1);
            source["meshes"][0]["primitives"][0]["attributes"].RemoveMember("TEXCOORD_0");
            source["materials"][0]["pbrMetallicRoughness"].RemoveMember("baseColorTexture");
        });
        const auto* model = load(GLTF);
        ASSERT_NE(model, nullptr) << messages_.str();
        ASSERT_EQ(model->geometries().size(), 1u);
        EXPECT_EQ(model->geometries()[0].vertices.size(), indexCount);
        ASSERT_EQ(model->geometries()[0].indices.size(), indexCount);
        EXPECT_EQ(model->geometries()[0].indices[vertexCount - 1], 65536u);
        EXPECT_EQ(model->geometries()[0].indices.back(), vertexCount);
        EXPECT_EQ(model->geometries()[0].vertices.back().position, model->geometries()[0].vertices[0].position);
        expectClosed();
    }

    TEST_F(ModelLoaderTest, RejectsVertexCountBeyondAssimpRepresentation)
    {
        editGltf([](auto& source) {
            source["accessors"][0]["count"].SetUint64(static_cast<uint64_t>(std::numeric_limits<unsigned>::max()) + 1);
        });
        expectFailure(GLTF, "Assimp vertex count range");
    }

    TEST_F(ModelLoaderTest, RejectsSourceIndexRepairEvenWhenOtherTrianglesAreValid)
    {
        auto bytes = read("models/minimal_gltf/geometry/mesh.bin");
        // 索引访问器 2 从字节偏移 80 开始；保留一个有效三角形和一个无效三角形。
        const uint16_t invalid = 65535;
        std::memcpy(bytes.data() + 80 + 5 * sizeof(uint16_t), &invalid, sizeof(invalid));
        write("models/minimal_gltf/geometry/mesh.bin", bytes);
        EXPECT_TRUE(Assimp::DefaultLogger::isNullLogger());
        expectFailure(GLTF, "out-of-range indices");
        EXPECT_TRUE(Assimp::DefaultLogger::isNullLogger());
    }

    TEST_F(ModelLoaderTest, RejectsSourceTriangleRemainderInsteadOfDroppingIt)
    {
        editGltf([](auto& source) { source["accessors"][2]["count"].SetUint(4); });
        expectFailure(GLTF, "TRIANGLES mode");
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

    TEST_F(ModelLoaderTest, LiteralPercentInModelDirectoryDoesNotDecodeAsDependencyUri)
    {
        fs::rename("assets/models/minimal_gltf", "assets/models/100%literal");
        ASSERT_NE(load("models/100%literal/model.gltf"), nullptr) << messages_.str();
        expectClosed();
    }

    TEST_F(ModelLoaderTest, ConvertsUint32SourceComponentsToInternalUint32)
    {
        auto bytes = read("models/minimal_gltf/geometry/mesh.bin");
        const std::array<uint32_t, 6> indices = {0, 1, 2, 0, 2, 3};
        bytes.replace(80, 12, reinterpret_cast<const char*>(indices.data()), sizeof(indices));
        write("models/minimal_gltf/geometry/mesh.bin", bytes);
        editGltf([&](auto& source) {
            source["buffers"][0]["byteLength"].SetUint64(bytes.size());
            source["bufferViews"][2]["byteLength"].SetUint(24);
            source["bufferViews"][3]["byteOffset"].SetUint(104);
            source["bufferViews"][4]["byteOffset"].SetUint(140);
            source["accessors"][2]["componentType"].SetUint(5125);
        });
        const auto* model = load(GLTF);
        ASSERT_NE(model, nullptr) << messages_.str();
        EXPECT_EQ(model->geometries()[0].indices, (std::vector<uint32_t>{0, 1, 2, 4, 5, 3}));
        expectClosed();
    }

    TEST_F(ModelLoaderTest, RequiredTextureTransformReportsMaterialContext)
    {
        editGltf([](auto& source) {
            auto& a = source.GetAllocator();
            rapidjson::Value required(rapidjson::kArrayType);
            required.PushBack("KHR_texture_transform", a);
            source.AddMember("extensionsRequired", required, a);
            rapidjson::Value extensions(rapidjson::kObjectType);
            extensions.AddMember("KHR_texture_transform", rapidjson::Value(rapidjson::kObjectType), a);
            source["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"].AddMember("extensions", extensions, a);
        });
        expectFailure(GLTF, "material 0");
        EXPECT_NE(messages_.str().find("UV transform"), std::string::npos);
    }

    TEST_F(ModelLoaderTest, ImportsAvocadoPbrTexturesFactorsAndOrthonormalTangents)
    {
        const auto* model = load("models/avocado/glTF/Avocado.gltf");
        ASSERT_NE(model, nullptr) << messages_.str();
        ASSERT_FALSE(model->parts().empty());
        const auto& mat = model->materials()[model->parts()[0].material];
        EXPECT_EQ(mat.texturePath, canonicalizeResourceKey("models/avocado/glTF/Avocado_baseColor.png"));
        EXPECT_EQ(mat.normalTexturePath, canonicalizeResourceKey("models/avocado/glTF/Avocado_normal.png"));
        EXPECT_EQ(mat.metallicRoughnessTexturePath,
                  canonicalizeResourceKey("models/avocado/glTF/Avocado_roughnessMetallic.png"));
        EXPECT_FLOAT_EQ(mat.normalScale, 1.0f);
        EXPECT_FLOAT_EQ(mat.metallic, 1.0f);
        EXPECT_FLOAT_EQ(mat.roughness, 1.0f);

        ASSERT_FALSE(model->geometries().empty());
        size_t validTangents = 0;
        for (const auto& vertex : model->geometries()[0].vertices)
        {
            const float nLen = std::hypot(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
            const float tLen = std::hypot(vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]);
            if (tLen > 0.0f)
            {
                ++validTangents;
                EXPECT_NEAR(tLen, 1.0f, 1e-4f);
                EXPECT_TRUE(vertex.tangent[3] == 1.0f || vertex.tangent[3] == -1.0f);
                if (nLen > 0.0f)
                {
                    const float dot = vertex.normal[0] * vertex.tangent[0] + vertex.normal[1] * vertex.tangent[1] +
                                      vertex.normal[2] * vertex.tangent[2];
                    EXPECT_NEAR(dot, 0.0f, 1e-4f);
                }
            }
        }
        EXPECT_GT(validTangents, 0u);
        expectClosed();
    }

    TEST_F(ModelLoaderTest, RejectsInvalidPbrTextureSlotsAndFactors)
    {
        const auto addSimpleNormalOrMr = [](rapidjson::Document& source, const char* slotName, const char* texName,
                                            bool inPbr) {
            auto& a = source.GetAllocator();
            const rapidjson::SizeType imgIdx = source["images"].Size();
            rapidjson::Value img(rapidjson::kObjectType);
            img.AddMember("uri", rapidjson::Value(texName, a), a);
            source["images"].PushBack(img, a);
            const rapidjson::SizeType texIdx = source["textures"].Size();
            rapidjson::Value tex(rapidjson::kObjectType);
            tex.AddMember("source", imgIdx, a);
            source["textures"].PushBack(tex, a);
            rapidjson::Value slot(rapidjson::kObjectType);
            slot.AddMember("index", texIdx, a);
            if (inPbr)
                source["materials"][0]["pbrMetallicRoughness"].AddMember(rapidjson::StringRef(slotName), slot, a);
            else
                source["materials"][0].AddMember(rapidjson::StringRef(slotName), slot, a);
        };
        // 复制一张有效 PNG 以便通过前置文件读取。
        write("models/minimal_gltf/textures/normal.png", read("models/minimal_gltf/textures/albedo.png"));

        // 1. normalTexture / metallicRoughnessTexture 使用 texCoord: 1
        for (const bool inPbr : {false, true})
        {
            const std::string saved = read(GLTF);
            editGltf([&](auto& source) {
                addSimpleNormalOrMr(source, inPbr ? "metallicRoughnessTexture" : "normalTexture",
                                    "textures/normal.png", inPbr);
                auto& target = inPbr ? source["materials"][0]["pbrMetallicRoughness"]["metallicRoughnessTexture"]
                                     : source["materials"][0]["normalTexture"];
                target.AddMember("texCoord", 1u, source.GetAllocator());
            });
            expectFailure(GLTF, "UV0");
            write(GLTF, saved);
        }

        // 2. normalTexture / metallicRoughnessTexture 使用 KHR_texture_transform
        for (const bool inPbr : {false, true})
        {
            const std::string saved = read(GLTF);
            editGltf([&](auto& source) {
                auto& a = source.GetAllocator();
                addSimpleNormalOrMr(source, inPbr ? "metallicRoughnessTexture" : "normalTexture",
                                    "textures/normal.png", inPbr);
                rapidjson::Value ext(rapidjson::kObjectType);
                ext.AddMember("KHR_texture_transform", rapidjson::Value(rapidjson::kObjectType), a);
                auto& target = inPbr ? source["materials"][0]["pbrMetallicRoughness"]["metallicRoughnessTexture"]
                                     : source["materials"][0]["normalTexture"];
                target.AddMember("extensions", ext, a);
            });
            expectFailure(GLTF, "UV transform");
            write(GLTF, saved);
        }

        // 3. 不支持的图片格式与路径逃逸。
        {
            const std::string saved = read(GLTF);
            editGltf([&](auto& source) {
                addSimpleNormalOrMr(source, "normalTexture", "textures/normal.webp", false);
            });
            expectFailure(GLTF, "PNG or JPEG");
            write(GLTF, saved);
        }
        {
            const std::string saved = read(GLTF);
            editGltf([&](auto& source) {
                addSimpleNormalOrMr(source, "metallicRoughnessTexture", "../../../outside.png", true);
            });
            expectFailure(GLTF, "escapes");
            write(GLTF, saved);
        }

        // 4. 缺失 TEXCOORD_0
        {
            const std::string saved = read(GLTF);
            editGltf([&](auto& source) {
                source["materials"][0]["pbrMetallicRoughness"].RemoveMember("baseColorTexture");
                addSimpleNormalOrMr(source, "normalTexture", "textures/normal.png", false);
                source["meshes"][0]["primitives"][0]["attributes"].RemoveMember("TEXCOORD_0");
            });
            expectFailure(GLTF, "UV0");
            write(GLTF, saved);
        }

        // 5. 非法因子
        for (const double bad : {-0.1, 1.2})
        {
            const std::string saved = read(GLTF);
            editGltf([&](auto& source) {
                source["materials"][0]["pbrMetallicRoughness"].AddMember("metallicFactor", bad, source.GetAllocator());
            });
            expectFailure(GLTF, "metallicFactor");
            write(GLTF, saved);
        }
        for (const double bad : {-0.1, 1.5})
        {
            const std::string saved = read(GLTF);
            editGltf([&](auto& source) {
                source["materials"][0]["pbrMetallicRoughness"].AddMember("roughnessFactor", bad, source.GetAllocator());
            });
            expectFailure(GLTF, "roughnessFactor");
            write(GLTF, saved);
        }
        {
            const std::string saved = read(GLTF);
            editGltf([&](auto& source) {
                addSimpleNormalOrMr(source, "normalTexture", "textures/normal.png", false);
                source["materials"][0]["normalTexture"].AddMember("scale", -0.5, source.GetAllocator());
            });
            expectFailure(GLTF, "normalTexture.scale");
            write(GLTF, saved);
        }
    }

    TEST_F(ModelLoaderTest, AcceptsZeroUv0AndOutputsZeroTangentForDegenerateUv)
    {
        auto bytes = read("models/minimal_gltf/geometry/mesh.bin");
        // UV0 位于字节偏移 48..80（4 个 vec2 = 32 字节），全部清零。
        std::fill(bytes.begin() + 48, bytes.begin() + 80, '\0');
        write("models/minimal_gltf/geometry/mesh.bin", bytes);
        const auto* model = load(GLTF);
        ASSERT_NE(model, nullptr) << messages_.str();
        for (const auto& vertex : model->geometries()[0].vertices)
        {
            EXPECT_EQ(vertex.uv, (std::array<float, 2>{0.0f, 0.0f}));
            EXPECT_EQ(vertex.tangent, (std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}));
        }
        expectClosed();
    }

    TEST_F(ModelLoaderTest, NativeAndGeneratedTangentsMatchExpectedBitangentDirectionIncludingMirroredUv)
    {
        // 在 XY 平面构造 4 顶点矩形（法线 +Z），分别测试：
        // 1) 无原生 TANGENT（由 ModelLoader 生成）
        // 2) 带原生 TANGENT
        // 3) 镜像 UV（u 翻转，手性应为相反符号）
        const auto writePlaneGltf = [&](std::string_view gltfPath, bool includeTangent, bool mirroredUv) {
            struct BinLayout
            {
                std::array<float, 12> pos;
                std::array<float, 8> uv;
                std::array<float, 12> norm;
                std::array<float, 16> tan;
                std::array<uint16_t, 6> idx;
            } bin{
                {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0},
                mirroredUv ? std::array<float, 8>{1, 1, 0, 1, 0, 0, 1, 0}
                           : std::array<float, 8>{0, 1, 1, 1, 1, 0, 0, 0},
                {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1},
                {mirroredUv ? -1.0f : 1.0f, 0, 0, mirroredUv ? 1.0f : -1.0f,
                 mirroredUv ? -1.0f : 1.0f, 0, 0, mirroredUv ? 1.0f : -1.0f,
                 mirroredUv ? -1.0f : 1.0f, 0, 0, mirroredUv ? 1.0f : -1.0f,
                 mirroredUv ? -1.0f : 1.0f, 0, 0, mirroredUv ? 1.0f : -1.0f},
                {0, 1, 2, 0, 2, 3},
            };
            write("models/minimal_gltf/geometry/mesh.bin",
                  std::string(reinterpret_cast<const char*>(&bin), sizeof(bin)));

            rapidjson::Document doc;
            doc.SetObject();
            auto& a = doc.GetAllocator();
            rapidjson::Value asset(rapidjson::kObjectType);
            asset.AddMember("version", "2.0", a);
            doc.AddMember("asset", asset, a);

            rapidjson::Value buffers(rapidjson::kArrayType);
            rapidjson::Value buf(rapidjson::kObjectType);
            buf.AddMember("uri", "geometry/mesh.bin", a);
            buf.AddMember("byteLength", static_cast<uint64_t>(sizeof(bin)), a);
            buffers.PushBack(buf, a);
            doc.AddMember("buffers", buffers, a);

            rapidjson::Value views(rapidjson::kArrayType);
            const auto addView = [&](uint64_t offset, uint64_t length) {
                rapidjson::Value v(rapidjson::kObjectType);
                v.AddMember("buffer", 0u, a);
                v.AddMember("byteOffset", offset, a);
                v.AddMember("byteLength", length, a);
                views.PushBack(v, a);
            };
            addView(offsetof(BinLayout, pos), sizeof(bin.pos));
            addView(offsetof(BinLayout, uv), sizeof(bin.uv));
            addView(offsetof(BinLayout, norm), sizeof(bin.norm));
            addView(offsetof(BinLayout, tan), sizeof(bin.tan));
            addView(offsetof(BinLayout, idx), sizeof(bin.idx));
            doc.AddMember("bufferViews", views, a);

            rapidjson::Value accessors(rapidjson::kArrayType);
            const auto addAccessor = [&](uint32_t view, uint32_t compType, uint32_t count, const char* type) {
                rapidjson::Value acc(rapidjson::kObjectType);
                acc.AddMember("bufferView", view, a);
                acc.AddMember("componentType", compType, a);
                acc.AddMember("count", count, a);
                acc.AddMember("type", rapidjson::StringRef(type), a);
                accessors.PushBack(acc, a);
            };
            addAccessor(0, 5126, 4, "VEC3");    // 0: POS
            addAccessor(1, 5126, 4, "VEC2");    // 1: UV
            addAccessor(2, 5126, 4, "VEC3");    // 2: NORM
            addAccessor(3, 5126, 4, "VEC4");    // 3: TAN
            addAccessor(4, 5123, 6, "SCALAR");  // 4: IDX
            doc.AddMember("accessors", accessors, a);

            rapidjson::Value attrs(rapidjson::kObjectType);
            attrs.AddMember("POSITION", 0u, a);
            attrs.AddMember("TEXCOORD_0", 1u, a);
            attrs.AddMember("NORMAL", 2u, a);
            if (includeTangent)
                attrs.AddMember("TANGENT", 3u, a);
            rapidjson::Value prim(rapidjson::kObjectType);
            prim.AddMember("attributes", attrs, a);
            prim.AddMember("indices", 4u, a);
            rapidjson::Value prims(rapidjson::kArrayType);
            prims.PushBack(prim, a);
            rapidjson::Value mesh(rapidjson::kObjectType);
            mesh.AddMember("primitives", prims, a);
            rapidjson::Value meshes(rapidjson::kArrayType);
            meshes.PushBack(mesh, a);
            doc.AddMember("meshes", meshes, a);

            rapidjson::Value node(rapidjson::kObjectType);
            node.AddMember("mesh", 0u, a);
            rapidjson::Value nodes(rapidjson::kArrayType);
            nodes.PushBack(node, a);
            doc.AddMember("nodes", nodes, a);
            rapidjson::Value sceneNodes(rapidjson::kArrayType);
            sceneNodes.PushBack(0u, a);
            rapidjson::Value scene(rapidjson::kObjectType);
            scene.AddMember("nodes", sceneNodes, a);
            rapidjson::Value scenes(rapidjson::kArrayType);
            scenes.PushBack(scene, a);
            doc.AddMember("scenes", scenes, a);
            doc.AddMember("scene", 0u, a);

            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            doc.Accept(writer);
            write(gltfPath, std::string(buffer.GetString(), buffer.GetSize()));
        };

        const auto checkPlane = [&](std::string_view gltfPath, bool includeTangent, bool mirroredUv) {
            writePlaneGltf(gltfPath, includeTangent, mirroredUv);
            const auto* model = load(gltfPath);
            ASSERT_NE(model, nullptr) << messages_.str();
            const float expectedTx = mirroredUv ? -1.0f : 1.0f;
            // v 从 1（y=0）减到 0（y=1），故 +V 方向指向 -Y（B = w * (N x T) = (0, -1, 0)）。
            // 对于正常 UV（T=+X, N=+Z），N x T = +Y，故 w = -1 使得 B = -Y；
            // 对于镜像 UV（T=-X, N=+Z），N x T = -Y，故 w = +1 使得 B = -Y。
            const float expectedW = mirroredUv ? 1.0f : -1.0f;
            for (const auto& vertex : model->geometries()[0].vertices)
            {
                EXPECT_NEAR(vertex.tangent[0], expectedTx, 1e-5f);
                EXPECT_NEAR(vertex.tangent[1], 0.0f, 1e-5f);
                EXPECT_NEAR(vertex.tangent[2], 0.0f, 1e-5f);
                EXPECT_FLOAT_EQ(vertex.tangent[3], expectedW);
            }
        };

        checkPlane("models/minimal_gltf/plane_gen.gltf", false, false);
        checkPlane("models/minimal_gltf/plane_nat.gltf", true, false);
        checkPlane("models/minimal_gltf/plane_mir_gen.gltf", false, true);
        checkPlane("models/minimal_gltf/plane_mir_nat.gltf", true, true);
        expectClosed();
    }

    TEST_F(ModelLoaderTest, MetallicFactorDefaultsAndObjPbrDefaults)
    {
        // glTF 省略 metallicFactor 时取 1.0（无论是否带 metallicRoughnessTexture）
        {
            const auto* model = load(GLTF);
            ASSERT_NE(model, nullptr);
            EXPECT_FLOAT_EQ(model->materials()[0].metallic, 1.0f);
            EXPECT_FLOAT_EQ(model->materials()[0].roughness, 1.0f);
            EXPECT_FLOAT_EQ(model->materials()[0].normalScale, 1.0f);
        }
        // glTF 显式 metallicFactor = 0.0 时保留 0.0
        {
            rapidjson::Document source;
            const std::string text = read(GLTF);
            source.Parse(text.data(), text.size());
            source["materials"][0]["pbrMetallicRoughness"].AddMember("metallicFactor", 0.0, source.GetAllocator());
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            source.Accept(writer);
            constexpr const char* explicitZeroPath = "models/minimal_gltf/model_metallic0.gltf";
            write(explicitZeroPath, std::string(buffer.GetString(), buffer.GetSize()));
            const auto* model = load(explicitZeroPath);
            ASSERT_NE(model, nullptr);
            EXPECT_FLOAT_EQ(model->materials()[0].metallic, 0.0f);
        }
        // OBJ 默认 normalScale = 1.0, metallic = 0.0, roughness = 0.45
        {
            const auto* obj = load(OBJ);
            ASSERT_NE(obj, nullptr);
            EXPECT_FLOAT_EQ(obj->materials()[0].normalScale, 1.0f);
            EXPECT_FLOAT_EQ(obj->materials()[0].metallic, 0.0f);
            EXPECT_FLOAT_EQ(obj->materials()[0].roughness, 0.45f);
            EXPECT_FALSE(obj->materials()[0].normalTexturePath.has_value());
            EXPECT_FALSE(obj->materials()[0].metallicRoughnessTexturePath.has_value());
        }
        expectClosed();
    }
}  // namespace
