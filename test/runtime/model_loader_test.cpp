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
            const auto utf8 = Utils::Utf8::toUtf8String(path);
            counters.paths.push_back(utf8);
            auto file = inner_->open(path, mode, share);
            if (!file)
                return nullptr;
            return std::make_unique<TrackedHandle>(
                std::move(file), counters,
                !counters.failPath.empty() && utf8.find(counters.failPath) != std::string::npos);
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
            ResourceManager::instance().registerLoader(ResourceType::MODEL, std::make_unique<ModelLoader>(fileIO_));
        }
        void TearDown() override
        {
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
        void editGltf(const std::function<void(rapidjson::Document&)>& edit)
        {
            rapidjson::Document source;
            const auto text = read(GLTF);
            source.Parse(text.c_str());
            ASSERT_FALSE(source.HasParseError());
            edit(source);
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            source.Accept(writer);
            write(GLTF, buffer.GetString());
        }
        ModelResource* load(std::string_view path)
        {
            id_ = ResourceManager::instance().registerResource(ResourceType::MODEL, path);
            return ResourceManager::instance().get<ModelResource>(id_);
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
            EXPECT_EQ(ResourceRegistry::instance().refCount(id_), 1u);
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
        ResourceId id_;
        std::ostringstream messages_;
    };

    TEST_F(ModelLoaderTest, GltfThroughManagerPreservesHierarchyPartsAndSharedMeshes)
    {
        const auto* model = load(GLTF);
        ASSERT_NE(model, nullptr) << messages_.str();
        EXPECT_EQ(model->type(), ResourceType::MODEL);
        EXPECT_EQ(model->state(), ResourceState::READY);
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
        EXPECT_EQ(model->geometries()[0].indices, (std::vector<uint32_t>{0, 1, 2, 0, 2, 3}));
        expectClosed();
        ResourceManager::instance().reset();
        fs::remove_all("assets/models");
        EXPECT_EQ(node(*model, "SourceRoot").name, "SourceRoot");
        EXPECT_FLOAT_EQ(model->geometries()[1].vertices[2].position[1], 0.5f);
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
        ResourceManager::instance().registerLoader(ResourceType::TEXTURE, std::make_unique<TextureLoader>(fileIO_));
        const auto textureId =
            ResourceManager::instance().registerResource(ResourceType::TEXTURE, "textures/direction_rgba.png");
        const auto* texture = ResourceManager::instance().get<TextureResource>(textureId);
        ASSERT_NE(texture, nullptr);
        EXPECT_GT(texture->width(), 1u);
        EXPECT_GT(texture->height(), 1u);
        // Distinct top and bottom rows, with V=0 at the upper mesh vertices in both formats.
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
        ModelLoader direct(fileIO_);
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
        ASSERT_NE(ResourceManager::instance().get<ModelResource>(failedId), nullptr);
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
            else if (name == "non_png")
            {
                source["images"][0]["uri"].SetString("../../textures/image.jpg", a);
                reason = "external PNG";
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
                                               "embedded_image", "embedded_buffer", "non_png", "animation", "skin",
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
        expectFailure(OBJ, "escapes assets root");
    }
    TEST_F(ModelLoaderTest, UnsupportedFileExtensionAndBrokenObjFail)
    {
        write("models/unsupported.glb", read(GLTF));
        expectFailure("models/unsupported.glb", "Only static");
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
            const std::array<float, 3> position = {static_cast<float>(vertex % 257),
                                                   static_cast<float>(vertex / 257), 0.0f};
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
        EXPECT_EQ(model->geometries()[0].vertices.size(), vertexCount);
        ASSERT_EQ(model->geometries()[0].indices.size(), indexCount);
        EXPECT_EQ(model->geometries()[0].indices[vertexCount - 1], 65536u);
        EXPECT_EQ(model->geometries()[0].indices.back(), 0u);
        expectClosed();
    }

    TEST_F(ModelLoaderTest, RejectsVertexCountBeyondAssimpRepresentation)
    {
        editGltf([](auto& source) {
            source["accessors"][0]["count"].SetUint64(
                static_cast<uint64_t>(std::numeric_limits<unsigned>::max()) + 1);
        });
        expectFailure(GLTF, "Assimp vertex count range");
    }

    TEST_F(ModelLoaderTest, RejectsSourceIndexRepairEvenWhenOtherTrianglesAreValid)
    {
        auto bytes = read("models/minimal_gltf/geometry/mesh.bin");
        // Index accessor 2 starts at byte 80; keep one good triangle and one bad triangle.
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
        EXPECT_EQ(model->geometries()[0].indices, (std::vector<uint32_t>{0, 1, 2, 0, 2, 3}));
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
}  // namespace
