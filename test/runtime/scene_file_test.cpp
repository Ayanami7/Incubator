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
        d.camera = Scene::Camera({3, 4, 5}, {2, 0, 1, 3}, {0.9f, 0.2f, 200});
        d.lighting = {{0, -2, -4}, {0.2f, 0.5f, 0.8f}, 2.25f, 0.3f};
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
        check(replace("\"visible\": false", "\"visible\": 0"), "visible");
        check(replace("\"mipMode\": \"fullChain\"", "\"mipMode\": \"other\""), "mipMode");
        check(replace("\"path\": \"models/兔子/模型.gltf\"", "\"path\": \"bad\\u0000path\""), "path");
        check(replace("\"clearColor\"", "\"missing\""), "clearColor");
        check(valid + "{}", "JSON");
        check(valid.substr(0, valid.size() - 1), "JSON");
        check(std::string("\xff", 1), "JSON");
        const std::string simple =
            R"({"version":1,"entities":[],"camera":{"position":[0,0,0],"rotation":[0,0,0,1],"perspective":{"verticalFov":0.8,"nearPlane":0.1,"farPlane":10}},"lighting":{"direction":[0,0,-1],"color":[1,1,1],"intensity":1,"ambient":0.1},"clearColor":[0,0,0,1]})";
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
}  // namespace
