#include "runtime/scene/scene_file.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

#include "runtime/core/error/error.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/scene/scene.h"
#include "runtime/scene/scene_rebuild.h"

namespace
{
    using namespace Incubator;
    namespace Fio = Incubator::FileIO;
    Scene::SceneDescription sample()
    {
        Scene::SceneDescription d;
        Model::PrepareOptions o{
            Render::TextureMipMode::FullChain,
            {Render::TextureFilter::Linear, Render::TextureFilter::Nearest, Render::MipFilter::Linear},
            Render::ShadingMode::Lambert};
        d.entities.push_back({Model::ModelReference("models/兔子/模型.gltf", o),
                              Scene::Transform({1.25f, -2, 3}, {2, 1, 0, 3}, {0, -2, 4}), false});
        d.entities.push_back({std::nullopt, Scene::Transform{}, true});
        d.entities[0].name = "兔子 ## 中文";
        d.entities[1].name = "### 空实体";
        d.camera = Scene::Camera({3, 4, 5}, {2, 0, 1, 3}, {0.9f, 0.2f, 200});
        d.lighting = {{0, -2, -4}, {0.2f, 0.5f, 0.8f}, 2.25f, 0.3f};
        d.display = {Render::ToneMappingMode::Off, 1.5f};
        d.clearColor = {0.1f, 0.2f, 0.3f, 0.4f};
        return d;
    }
    void compare(const Scene::SceneDescription& a, const Scene::SceneDescription& b)
    {
        ASSERT_EQ(a.entities.size(), b.entities.size());
        for (size_t i = 0; i < a.entities.size(); ++i)
        {
            EXPECT_EQ(a.entities[i].model, b.entities[i].model);
            EXPECT_EQ(a.entities[i].visible, b.entities[i].visible);
            EXPECT_EQ(a.entities[i].name, b.entities[i].name);
            EXPECT_EQ(a.entities[i].parent, b.entities[i].parent);
            EXPECT_EQ(a.entities[i].transform.position(), b.entities[i].transform.position());
            EXPECT_EQ(a.entities[i].transform.scale(), b.entities[i].transform.scale());
            EXPECT_NEAR(std::abs(glm::dot(a.entities[i].transform.rotation(), b.entities[i].transform.rotation())), 1,
                        1e-5);
        }
        EXPECT_EQ(a.camera.position(), b.camera.position());
        EXPECT_NEAR(std::abs(glm::dot(a.camera.rotation(), b.camera.rotation())), 1, 1e-5);
        EXPECT_FLOAT_EQ(a.camera.perspective().verticalFov, b.camera.perspective().verticalFov);
        EXPECT_FLOAT_EQ(a.camera.perspective().nearPlane, b.camera.perspective().nearPlane);
        EXPECT_FLOAT_EQ(a.camera.perspective().farPlane, b.camera.perspective().farPlane);
        EXPECT_EQ(a.lighting, b.lighting);
        EXPECT_EQ(a.display, b.display);
        EXPECT_EQ(a.clearColor, b.clearColor);
    }
    TEST(SceneFileTest, RoundTripAndAllOptions)
    {
        auto d = sample();
        compare(d, Scene::decodeScene(Scene::encodeScene(d)));
        for (auto mip : {Render::TextureMipMode::BaseLevel, Render::TextureMipMode::FullChain})
            for (auto min : {Render::TextureFilter::Nearest, Render::TextureFilter::Linear})
                for (auto mag : {Render::TextureFilter::Nearest, Render::TextureFilter::Linear})
                    for (auto filter :
                         {Render::MipFilter::BaseLevel, Render::MipFilter::Nearest, Render::MipFilter::Linear})
                        for (auto shade : {Render::ShadingMode::Unlit, Render::ShadingMode::Lambert})
                        {
                            d.entities[0].model =
                                Model::ModelReference("models/a.gltf", {mip, {min, mag, filter}, shade});
                            compare(d, Scene::decodeScene(Scene::encodeScene(d)));
                        }
        d.entities.clear();
        compare(d, Scene::decodeScene(Scene::encodeScene(d)));
    }
    TEST(SceneFileTest, RejectsInvalidJsonWithContext)
    {
        const auto valid = Scene::encodeScene(sample());
        auto check = [&](std::string json, std::string_view context) {
            try
            {
                (void)Scene::decodeScene(json);
                FAIL() << "accepted " << json;
            }
            catch (const Error::Exception& e)
            {
                EXPECT_EQ(e.code(), Error::Code::IO);
                EXPECT_NE(e.message().find(context), std::string::npos) << e.message();
            }
        };
        auto replace = [&](std::string from, std::string to) {
            auto text = valid;
            auto at = text.find(from);
            EXPECT_NE(at, std::string::npos);
            if (at != std::string::npos)
                text.replace(at, from.size(), to);
            return text;
        };
        check(replace("\"version\": 1", "\"version\": 2"), "version");
        check(replace("\"version\": 1", "\"version\": 1.0"), "version");
        check(replace("\"minorVersion\": 1", "\"minorVersion\": 2"), "minorVersion");
        check(replace("\"minorVersion\": 1", "\"minorVersion\": -1"), "minorVersion");
        check(replace("\"minorVersion\": 1", "\"minorVersion\": 1.0"), "minorVersion");
        check(replace("\"visible\": false", "\"visible\": 0"), "visible");
        check(replace("\"name\": \"兔子 ## 中文\"", "\"name\": 42"), "entities[0].name");
        check(replace("\"name\": \"兔子 ## 中文\"", "\"name\": \"bad\\u0000name\""), "entities[0].name");
        auto legacy = valid;
        const auto nameAt = legacy.find("\"name\": \"兔子 ## 中文\",");
        ASSERT_NE(nameAt, std::string::npos);
        legacy.erase(nameAt, std::string("\"name\": \"兔子 ## 中文\",").size());
        EXPECT_TRUE(Scene::decodeScene(legacy).entities[0].name.empty());
        auto invalid = sample();
        invalid.entities[0].name = std::string("\xff", 1);
        EXPECT_THROW(Scene::encodeScene(invalid), Error::Exception);
        check(replace("\"mipMode\": \"fullChain\"", "\"mipMode\": \"other\""), "mipMode");
        check(replace("\"path\": \"models/兔子/模型.gltf\"", "\"path\": \"bad\\u0000path\""), "path");
        check(replace("\"clearColor\"", "\"missing\""), "clearColor");
        check(valid + "{}", "JSON");
        check(valid.substr(0, valid.size() - 1), "JSON");
        check(std::string("\xff", 1), "JSON");
        const std::string simple =
            R"({"version":1,"entities":[],"camera":{"position":[0,0,0],"rotation":[0,0,0,1],"perspective":{"verticalFov":0.8,"nearPlane":0.1,"farPlane":10}},"lighting":{"direction":[0,0,-1],"color":[1,1,1],"intensity":1,"ambient":0.1},"display":{"toneMapping":"reinhard","exposureEv":0},"clearColor":[0,0,0,1]})";
        auto bad = [&](std::string from, std::string to, std::string_view field) {
            auto text = simple;
            const auto at = text.find(from);
            ASSERT_NE(at, std::string::npos);
            text.replace(at, from.size(), to);
            check(text, field);
        };
        bad("[0,0,0]", "[0,0]", "position");
        bad("[0,0,0,1]", "[0,0,0,0]", "camera");
        bad("\"verticalFov\":0.8", "\"verticalFov\":1e100", "verticalFov");
        bad("\"direction\":[0,0,-1]", "\"direction\":[0,0,0]", "lighting");
        bad("\"display\":{\"toneMapping\":\"reinhard\",\"exposureEv\":0},", "", "display");
        bad("\"toneMapping\":\"reinhard\",", "", "display.toneMapping");
        bad(",\"exposureEv\":0", "", "display.exposureEv");
        bad("\"toneMapping\":\"reinhard\"", "\"toneMapping\":\"aces\"", "display.toneMapping");
        bad("\"exposureEv\":0", "\"exposureEv\":\"0\"", "display.exposureEv");
        bad("\"exposureEv\":0", "\"exposureEv\":1e100", "display.exposureEv");
        bad("\"exposureEv\":0", "\"exposureEv\":-8.1", "display.exposureEv");
        bad("\"exposureEv\":0", "\"exposureEv\":8.1", "display.exposureEv");
        bad("\"clearColor\":[0,0,0,1]", "\"clearColor\":[0,0,0,2]", "clearColor");
        auto unknown = valid;
        unknown.insert(1, "\"extra\": true,");
        compare(sample(), Scene::decodeScene(unknown));
    }
    struct State
    {
        std::string data;
        size_t pos = 0;
        size_t chunk = 3;
        bool zero = false, failRead = false, failWrite = false, failFlush = false;
        int opens = 0;
    };
    class Handle final : public Fio::FileHandle
    {
    public:
        explicit Handle(State& state) : s(state) {}
        bool isValid() const override
        {
            return true;
        }
        size_t read(std::span<uint8_t> out) override
        {
            if (s.failRead)
                throw Error::Exception(Error::Code::IO, "read failure");
            if (s.zero)
                return 0;
            auto n = std::min({out.size(), s.chunk, s.data.size() - s.pos});
            std::copy_n(s.data.data() + s.pos, n, out.data());
            s.pos += n;
            return n;
        }
        size_t write(std::span<const uint8_t> in) override
        {
            if (s.failWrite)
                throw Error::Exception(Error::Code::IO, "write failure");
            if (s.zero)
                return 0;
            auto n = std::min(in.size(), s.chunk);
            s.data.append(reinterpret_cast<const char*>(in.data()), n);
            return n;
        }
        size_t seek(int64_t, Fio::SeekOrigin) override
        {
            return 0;
        }
        uint64_t tell() const override
        {
            return s.pos;
        }
        uint64_t size() const override
        {
            return s.data.size();
        }
        void close() override {}
        void flush() override
        {
            if (s.failFlush)
                throw Error::Exception(Error::Code::IO, "flush failure");
        }

    private:
        State& s;
    };
    class MemoryIO final : public Fio::FileIO
    {
    public:
        State state;
        bool exists(const std::filesystem::path&) override
        {
            return false;
        }
        uint64_t getFileSize(const std::filesystem::path&) override
        {
            return 0;
        }
        bool remove(const std::filesystem::path&) override
        {
            return false;
        }
        std::vector<Fio::DirectoryEntry> listDirectory(const std::filesystem::path&) override
        {
            throw Error::Exception(Error::Code::IO, "Memory scene IO does not enumerate directories");
        }
        bool directoryExists(const std::filesystem::path&) override
        {
            return false;
        }
        bool createDirectory(const std::filesystem::path&) override
        {
            return false;
        }
        bool removeDirectory(const std::filesystem::path&) override
        {
            return false;
        }
        std::unique_ptr<Fio::FileHandle> open(const std::filesystem::path&, Fio::OpenMode mode, Fio::ShareMode) override
        {
            ++state.opens;
            state.pos = 0;
            if (Fio::hasFlag(mode, Fio::OpenMode::Truncate))
                state.data.clear();
            return std::make_unique<Handle>(state);
        }
    };
    TEST(SceneFileTest, ShortIoAndFailures)
    {
        MemoryIO io;
        const auto d = sample();
        Scene::writeScene(io, "场景.json", d);
        compare(d, Scene::readScene(io, "场景.json"));
        io.state.zero = true;
        EXPECT_THROW(Scene::readScene(io, "场景.json"), Error::Exception);
        EXPECT_THROW(Scene::writeScene(io, "场景.json", d), Error::Exception);
        io.state.zero = false;
        io.state.data = Scene::encodeScene(d);
        io.state.failRead = true;
        EXPECT_THROW(Scene::readScene(io, "场景.json"), Error::Exception);
        io.state.failRead = false;
        io.state.failWrite = true;
        EXPECT_THROW(Scene::writeScene(io, "场景.json", d), Error::Exception);
        io.state.failWrite = false;
        io.state.failFlush = true;
        EXPECT_THROW(Scene::writeScene(io, "场景.json", d), Error::Exception);
        io.state.failFlush = false;
        auto invalid = d;
        invalid.lighting.direction = {0, 0, 0};
        const auto opens = io.state.opens;
        try
        {
            Scene::writeScene(io, "场景.json", invalid);
            FAIL();
        }
        catch (const Error::Exception& e)
        {
            EXPECT_EQ(e.code(), Error::Code::CONFIG);
        }
        EXPECT_EQ(io.state.opens, opens);
    }
    TEST(SceneFileTest, RealUnicodeFileCreateAndOverwrite)
    {
        auto io = Fio::createPlatformFileIO();
        const auto cwd = std::filesystem::current_path();
        const auto p = cwd / Utils::Utf8::toFsPath("scene 测试 😀.json");
        const auto name = Utils::Utf8::toUtf8String(p);
        Scene::writeScene(*io, name, sample());
        compare(sample(), Scene::readScene(*io, name));
        Scene::SceneDescription empty;
        Scene::writeScene(*io, name, empty);
        compare(empty, Scene::readScene(*io, name));
        EXPECT_EQ(std::filesystem::current_path(), cwd);
        EXPECT_TRUE(io->remove(p));
    }

    TEST(SceneFileTest, DeployedFixturesDecodeAndReportFieldContext)
    {
        auto io = Fio::createPlatformFileIO();
        const auto base = std::filesystem::path(SCENE_FIXTURE_DIR);
        const auto loaded = Scene::readScene(*io, Utils::Utf8::toUtf8String(base / "mixed.json"));
        ASSERT_EQ(loaded.entities.size(), 2u);
        ASSERT_TRUE(loaded.entities[0].model);
        EXPECT_EQ(loaded.entities[0].model->path(), "models/兔子/模型.gltf");
        EXPECT_FALSE(loaded.entities[1].model);
        try
        {
            (void)Scene::readScene(*io, Utils::Utf8::toUtf8String(base / "invalid-camera.json"));
            FAIL() << "accepted invalid fixture";
        }
        catch (const Error::Exception& e)
        {
            EXPECT_EQ(e.code(), Error::Code::IO);
            EXPECT_NE(e.message().find("camera"), std::string::npos);
            EXPECT_NE(e.message().find("invalid-camera.json"), std::string::npos);
        }
    }

    TEST(SceneFileTest, RoundTripsPbrShadingAndRejectsInvalidShading)
    {
        auto d = sample();
        d.entities[0].model = Model::ModelReference(
            "models/avocado/glTF/Avocado.gltf",
            {Render::TextureMipMode::FullChain,
             {Render::TextureFilter::Linear, Render::TextureFilter::Linear, Render::MipFilter::Linear},
             Render::ShadingMode::Pbr});
        const std::string encoded = Scene::encodeScene(d);
        EXPECT_NE(encoded.find("\"pbr\""), std::string::npos);
        const auto decoded = Scene::decodeScene(encoded);
        compare(d, decoded);
        ASSERT_TRUE(decoded.entities[0].model.has_value());
        EXPECT_EQ(decoded.entities[0].model->options().shading, Render::ShadingMode::Pbr);

        std::string invalid = encoded;
        const size_t pos = invalid.find("\"pbr\"");
        ASSERT_NE(pos, std::string::npos);
        invalid.replace(pos, 5, "\"phong\"");
        EXPECT_THROW((void)Scene::decodeScene(invalid), Error::Exception);
    }

    TEST(SceneFileTest, RoundTripsDisplaySettingsAndReadsUpdatedBuiltinAssets)
    {
        auto d = sample();
        for (const auto& display :
             std::array<Render::DisplaySettings, 4>{{{Render::ToneMappingMode::Off, -3.25f},
                                                     {Render::ToneMappingMode::Off, 8.0f},
                                                     {Render::ToneMappingMode::Reinhard, -8.0f},
                                                     {Render::ToneMappingMode::Reinhard, 2.5f}}})
        {
            d.display = display;
            compare(d, Scene::decodeScene(Scene::encodeScene(d)));
        }

        auto io = Fio::createPlatformFileIO();
        for (const char* name : {"mixed.json", "shadows.json"})
        {
            const auto path = std::filesystem::path(SCENE_FIXTURE_DIR).parent_path().parent_path().parent_path() /
                              "assets/scenes" / name;
            if (std::filesystem::exists(path))
            {
                const auto loaded = Scene::readScene(*io, Utils::Utf8::toUtf8String(path));
                EXPECT_EQ(loaded.display, (Render::DisplaySettings{Render::ToneMappingMode::Reinhard, 0.0f}));
            }
        }
        {
            const auto showcasePath =
                std::filesystem::path(SCENE_FIXTURE_DIR).parent_path().parent_path().parent_path() /
                "assets/scenes/pbr-showcase.json";
            ASSERT_TRUE(std::filesystem::exists(showcasePath));
            const auto showcase = Scene::readScene(*io, Utils::Utf8::toUtf8String(showcasePath));
            ASSERT_EQ(showcase.entities.size(), 3u);
            EXPECT_EQ(showcase.entities[0].name, "Avocado Cut");
            EXPECT_EQ(showcase.entities[1].name, "Avocado Peel");
            EXPECT_EQ(showcase.entities[2].name, "Ground");
            for (const auto& entity : showcase.entities)
            {
                EXPECT_FALSE(entity.parent.has_value());
                ASSERT_TRUE(entity.model.has_value());
                EXPECT_EQ(entity.model->options().mipMode, Render::TextureMipMode::FullChain);
                EXPECT_EQ(entity.model->options().sampler.minFilter, Render::TextureFilter::Linear);
                EXPECT_EQ(entity.model->options().sampler.magFilter, Render::TextureFilter::Linear);
                EXPECT_EQ(entity.model->options().sampler.mipFilter, Render::MipFilter::Linear);
                EXPECT_EQ(entity.model->options().shading, Render::ShadingMode::Pbr);
            }
            EXPECT_EQ(showcase.display.toneMapping, Render::ToneMappingMode::Reinhard);
            EXPECT_FLOAT_EQ(showcase.display.exposureEv, 0.15f);
        }
    }

    TEST(SceneFileTest, HierarchyVersion2RoundTripForwardParentDuplicateNamesAndInvalidParentErrors)
    {
        // 构造父节点在子节点之后且包含重名实体的 v2 场景，验证往返、重建及世界姿态保持。
        Scene::Scene source;
        source.setDisplay({Render::ToneMappingMode::Off, 1.25f});
        const auto child = source.createEntity({}, Scene::Transform({1.0f, 0.0f, 0.0f}), true, "Node");
        const auto parent = source.createEntity(
            {}, Scene::Transform({0.0f, 2.0f, 0.0f}, glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 1, 0))), true,
            "Node");
        ASSERT_TRUE(source.setParent(child, parent));
        const auto expectedWorld = *source.worldMatrix(child);

        auto desc = Scene::captureScene(source, {0.2f, 0.3f, 0.4f, 1.0f});
        ASSERT_EQ(desc.entities[0].parent, 1u);
        ASSERT_FALSE(desc.entities[1].parent.has_value());

        const std::string json = Scene::encodeScene(desc);
        EXPECT_NE(json.find("\"version\": 1"), std::string::npos);
        EXPECT_NE(json.find("\"minorVersion\": 1"), std::string::npos);
        const auto decoded = Scene::decodeScene(json);
        compare(desc, decoded);

        int resolveCalls = 0;
        const auto rebuilt = Scene::rebuildScene(decoded, [&](const Model::ModelReference&) {
            ++resolveCalls;
            return std::shared_ptr<const Model::PreparedModel>{};
        });
        EXPECT_EQ(resolveCalls, 0);
        ASSERT_EQ(rebuilt.scene->entities().size(), 2u);
        EXPECT_EQ(rebuilt.scene->entities()[0].name, "Node");
        EXPECT_EQ(rebuilt.scene->entities()[1].name, "Node");
        EXPECT_EQ(rebuilt.scene->entities()[0].parent, rebuilt.scene->entities()[1].id);
        EXPECT_FALSE(rebuilt.scene->entities()[1].parent.valid());
        const auto rebuiltWorld = *rebuilt.scene->worldMatrix(rebuilt.scene->entities()[0].id);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                EXPECT_NEAR(rebuiltWorld[c][r], expectedWorld[c][r], 1e-5f);
        EXPECT_EQ(rebuilt.scene->display(), (Render::DisplaySettings{Render::ToneMappingMode::Off, 1.25f}));

        // 验证 v2 缺失 parent、非整数、越界、自指及循环均报 entities[i].parent 错误。
        const auto expectBadJson = [&](std::string text, std::string_view expectedField) {
            try
            {
                (void)Scene::decodeScene(text);
                FAIL() << "accepted invalid hierarchy JSON";
            }
            catch (const Error::Exception& e)
            {
                EXPECT_EQ(e.code(), Error::Code::IO);
                EXPECT_NE(e.message().find(expectedField), std::string::npos) << e.message();
            }
        };
        auto replaceFirst = [&](std::string text, std::string_view from, std::string_view to) {
            const auto pos = text.find(from);
            EXPECT_NE(pos, std::string::npos);
            if (pos != std::string::npos)
                text.replace(pos, from.size(), to);
            return text;
        };
        expectBadJson(replaceFirst(json, "\"parent\": 1,", ""), "entities[0].parent");
        expectBadJson(replaceFirst(json, "\"parent\": 1", "\"parent\": 1.5"), "entities[0].parent");
        expectBadJson(replaceFirst(json, "\"parent\": 1", "\"parent\": \"1\""), "entities[0].parent");
        expectBadJson(replaceFirst(json, "\"parent\": 1", "\"parent\": -1"), "entities[0].parent");
        expectBadJson(replaceFirst(json, "\"parent\": 1", "\"parent\": 99"), "entities[0].parent");
        expectBadJson(replaceFirst(json, "\"parent\": 1", "\"parent\": 0"), "entities[0].parent");
        expectBadJson(replaceFirst(json, "\"parent\": null", "\"parent\": 0"), "entities[0].parent");

        // 直接重建遇到非法层级应在解析模型前失败。
        auto invalidDesc = desc;
        invalidDesc.entities[0].model = Model::ModelReference("models/a.gltf");
        invalidDesc.entities[1].parent = 0u;
        EXPECT_FALSE(invalidDesc.valid());
        int badResolveCalls = 0;
        try
        {
            (void)Scene::rebuildScene(invalidDesc, [&](const Model::ModelReference&) {
                ++badResolveCalls;
                return std::shared_ptr<const Model::PreparedModel>{};
            });
            FAIL() << "accepted invalid hierarchy description";
        }
        catch (const Error::Exception& e)
        {
            EXPECT_EQ(e.code(), Error::Code::CONFIG);
            EXPECT_NE(e.message().find("entities[0].parent"), std::string::npos) << e.message();
        }
        EXPECT_EQ(badResolveCalls, 0);
    }
}  // namespace
