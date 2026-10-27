#include "runtime/scene/scene_file.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <cmath>
#include <limits>
#include <span>
#include <string>

#include "runtime/core/error/error.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/platform/fileio/fileio.h"

namespace Incubator::Scene
{
    namespace
    {
        using Value = rapidjson::Value;
        using Writer = rapidjson::PrettyWriter<rapidjson::StringBuffer>;
        [[noreturn]] void fail(std::string_view field, std::string_view reason)
        {
            throw Error::Exception(Error::Code::IO, fmt::format("{}: {}", field, reason));
        }
        const Value& member(const Value& parent, const char* key, const std::string& where)
        {
            if (!parent.IsObject())
                fail(where, "expected object");
            const auto it = parent.FindMember(key);
            if (it == parent.MemberEnd())
                fail(where + "." + key, "missing field");
            return it->value;
        }
        float number(const Value& v, const std::string& where)
        {
            if (!v.IsNumber())
                fail(where, "expected number");
            const double n = v.GetDouble();
            if (!std::isfinite(n) || std::abs(n) > std::numeric_limits<float>::max())
                fail(where, "number out of range");
            return static_cast<float>(n);
        }
        template <size_t N>
        std::array<float, N> array(const Value& v, const std::string& where)
        {
            if (!v.IsArray() || v.Size() != N)
                fail(where, "invalid array length or type");
            std::array<float, N> result{};
            for (size_t i = 0; i < N; ++i)
                result[i] = number(v[static_cast<rapidjson::SizeType>(i)], where + "[" + std::to_string(i) + "]");
            return result;
        }
        glm::vec3 vec3(const Value& v, const std::string& where)
        {
            const auto a = array<3>(v, where);
            return {a[0], a[1], a[2]};
        }
        glm::quat quat(const Value& v, const std::string& where)
        {
            const auto a = array<4>(v, where);
            return {a[3], a[0], a[1], a[2]};
        }
        std::string str(const Value& v, const std::string& where)
        {
            if (!v.IsString())
                fail(where, "expected string");
            return {v.GetString(), v.GetStringLength()};
        }
        template <typename T>
        T choice(const Value& v, const std::string& where,
                 std::initializer_list<std::pair<std::string_view, T>> choices)
        {
            const auto s = str(v, where);
            for (const auto& [name, result] : choices)
                if (s == name)
                    return result;
            fail(where, "unknown enum value");
        }
        template <typename F>
        auto checked(const std::string& field, F&& f)
        {
            try
            {
                return f();
            }
            catch (const Error::Exception& e)
            {
                fail(field, e.message());
            }
        }
        Model::ModelReference model(const Value& v, const std::string& p)
        {
            using namespace Render;
            const auto path = str(member(v, "path", p), p + ".path");
            const auto& prep = member(v, "prepare", p);
            const auto q = p + ".prepare";
            const auto& sampler = member(prep, "sampler", q);
            const auto s = q + ".sampler";
            Model::PrepareOptions o;
            o.mipMode = choice<TextureMipMode>(
                member(prep, "mipMode", q), q + ".mipMode",
                {{"baseLevel", TextureMipMode::BaseLevel}, {"fullChain", TextureMipMode::FullChain}});
            o.sampler.minFilter =
                choice<TextureFilter>(member(sampler, "minFilter", s), s + ".minFilter",
                                      {{"nearest", TextureFilter::Nearest}, {"linear", TextureFilter::Linear}});
            o.sampler.magFilter =
                choice<TextureFilter>(member(sampler, "magFilter", s), s + ".magFilter",
                                      {{"nearest", TextureFilter::Nearest}, {"linear", TextureFilter::Linear}});
            o.sampler.mipFilter = choice<MipFilter>(
                member(sampler, "mipFilter", s), s + ".mipFilter",
                {{"baseLevel", MipFilter::BaseLevel}, {"nearest", MipFilter::Nearest}, {"linear", MipFilter::Linear}});
            o.shading = choice<ShadingMode>(member(prep, "shading", q), q + ".shading",
                                            {{"unlit", ShadingMode::Unlit}, {"lambert", ShadingMode::Lambert}});
            return checked(p + ".path", [&] { return Model::ModelReference(path, o); });
        }
        void vec(Writer& w, glm::vec3 v)
        {
            w.StartArray();
            w.Double(v.x);
            w.Double(v.y);
            w.Double(v.z);
            w.EndArray();
        }
        void rotation(Writer& w, glm::quat q)
        {
            w.StartArray();
            w.Double(q.x);
            w.Double(q.y);
            w.Double(q.z);
            w.Double(q.w);
            w.EndArray();
        }
        void key(Writer& w, const char* name)
        {
            w.Key(name);
        }
        void text(Writer& w, std::string_view s)
        {
            w.String(s.data(), static_cast<rapidjson::SizeType>(s.size()));
        }
        std::filesystem::path filePath(std::string_view path)
        {
            if (path.empty() || path.find('\0') != std::string_view::npos || !Utils::Utf8::isValid(path))
                throw Error::Exception(Error::Code::IO, "Invalid UTF-8 scene file path");
            return Utils::Utf8::toFsPath(path);
        }
    }  // namespace

    std::string encodeScene(const SceneDescription& d)
    {
        if (!d.valid())
            throw Error::Exception(Error::Code::CONFIG, "Invalid scene lighting or clearColor");
        for (const auto& entity : d.entities)
        {
            if (!entity.model)
                continue;
            try
            {
                if (Model::ModelReference(entity.model->path(), entity.model->options()) != *entity.model)
                    throw Error::Exception(Error::Code::CONFIG, "Invalid scene model reference");
            }
            catch (const Error::Exception& e)
            {
                throw Error::Exception(Error::Code::CONFIG,
                                       fmt::format("Invalid scene model reference: {}", e.message()));
            }
        }
        rapidjson::StringBuffer buffer;
        Writer w(buffer);
        w.StartObject();
        key(w, "version");
        w.Int(1);
        key(w, "entities");
        w.StartArray();
        for (const auto& e : d.entities)
        {
            w.StartObject();
            key(w, "model");
            if (!e.model)
                w.Null();
            else
            {
                const auto& o = e.model->options();
                w.StartObject();
                key(w, "path");
                text(w, e.model->path());
                key(w, "prepare");
                w.StartObject();
                key(w, "mipMode");
                text(w, o.mipMode == Render::TextureMipMode::BaseLevel ? "baseLevel" : "fullChain");
                key(w, "sampler");
                w.StartObject();
                key(w, "minFilter");
                text(w, o.sampler.minFilter == Render::TextureFilter::Nearest ? "nearest" : "linear");
                key(w, "magFilter");
                text(w, o.sampler.magFilter == Render::TextureFilter::Nearest ? "nearest" : "linear");
                key(w, "mipFilter");
                text(w, o.sampler.mipFilter == Render::MipFilter::BaseLevel ? "baseLevel"
                        : o.sampler.mipFilter == Render::MipFilter::Nearest ? "nearest"
                                                                            : "linear");
                w.EndObject();
                key(w, "shading");
                text(w, o.shading == Render::ShadingMode::Unlit ? "unlit" : "lambert");
                w.EndObject();
                w.EndObject();
            }
            key(w, "transform");
            w.StartObject();
            key(w, "position");
            vec(w, e.transform.position());
            key(w, "rotation");
            rotation(w, e.transform.rotation());
            key(w, "scale");
            vec(w, e.transform.scale());
            w.EndObject();
            key(w, "visible");
            w.Bool(e.visible);
            w.EndObject();
        }
        w.EndArray();
        key(w, "camera");
        w.StartObject();
        key(w, "position");
        vec(w, d.camera.position());
        key(w, "rotation");
        rotation(w, d.camera.rotation());
        key(w, "perspective");
        w.StartObject();
        key(w, "verticalFov");
        w.Double(d.camera.perspective().verticalFov);
        key(w, "nearPlane");
        w.Double(d.camera.perspective().nearPlane);
        key(w, "farPlane");
        w.Double(d.camera.perspective().farPlane);
        w.EndObject();
        w.EndObject();
        key(w, "lighting");
        w.StartObject();
        key(w, "direction");
        vec(w, d.lighting.direction);
        key(w, "color");
        vec(w, d.lighting.color);
        key(w, "intensity");
        w.Double(d.lighting.intensity);
        key(w, "ambient");
        w.Double(d.lighting.ambient);
        w.EndObject();
        key(w, "clearColor");
        w.StartArray();
        for (float c : d.clearColor)
            w.Double(c);
        w.EndArray();
        w.EndObject();
        return {buffer.GetString(), buffer.GetSize()};
    }

    SceneDescription decodeScene(std::string_view json)
    {
        if (!Utils::Utf8::isValid(json) || json.find('\0') != std::string_view::npos)
            fail("JSON", "invalid UTF-8 or embedded NUL");
        if (json.starts_with("\xef\xbb\xbf"))
            fail("JSON", "BOM is not supported");
        rapidjson::Document doc;
        doc.Parse<rapidjson::kParseValidateEncodingFlag>(json.data(), json.size());
        if (doc.HasParseError())
            fail("JSON",
                 fmt::format("{} at byte {}", rapidjson::GetParseError_En(doc.GetParseError()), doc.GetErrorOffset()));
        const auto& version = member(doc, "version", "root");
        if (!version.IsInt() || version.GetInt() != 1)
            fail("version", "expected integer 1");
        const auto& entities = member(doc, "entities", "root");
        if (!entities.IsArray())
            fail("entities", "expected array");
        SceneDescription result;
        for (rapidjson::SizeType i = 0; i < entities.Size(); ++i)
        {
            const auto p = "entities[" + std::to_string(i) + "]";
            const auto& e = entities[i];
            const auto& m = member(e, "model", p);
            EntityDescription item;
            if (!m.IsNull())
                item.model = model(m, p + ".model");
            const auto& t = member(e, "transform", p);
            const auto tp = p + ".transform";
            const auto position = vec3(member(t, "position", tp), tp + ".position");
            const auto r = quat(member(t, "rotation", tp), tp + ".rotation");
            const auto scale = vec3(member(t, "scale", tp), tp + ".scale");
            item.transform = checked(tp, [&] { return Transform(position, r, scale); });
            const auto& visible = member(e, "visible", p);
            if (!visible.IsBool())
                fail(p + ".visible", "expected boolean");
            item.visible = visible.GetBool();
            result.entities.push_back(std::move(item));
        }
        const auto& c = member(doc, "camera", "root");
        const auto cp = vec3(member(c, "position", "camera"), "camera.position");
        const auto cr = quat(member(c, "rotation", "camera"), "camera.rotation");
        const auto& persp = member(c, "perspective", "camera");
        const Perspective p{
            number(member(persp, "verticalFov", "camera.perspective"), "camera.perspective.verticalFov"),
            number(member(persp, "nearPlane", "camera.perspective"), "camera.perspective.nearPlane"),
            number(member(persp, "farPlane", "camera.perspective"), "camera.perspective.farPlane")};
        result.camera = checked("camera", [&] { return Camera(cp, cr, p); });
        const auto& l = member(doc, "lighting", "root");
        result.lighting = {vec3(member(l, "direction", "lighting"), "lighting.direction"),
                           vec3(member(l, "color", "lighting"), "lighting.color"),
                           number(member(l, "intensity", "lighting"), "lighting.intensity"),
                           number(member(l, "ambient", "lighting"), "lighting.ambient")};
        if (!result.lighting.valid())
            fail("lighting", "invalid value");
        result.clearColor = array<4>(member(doc, "clearColor", "root"), "clearColor");
        if (!result.valid())
            fail("clearColor", "values must be in [0, 1]");
        return result;
    }

    SceneDescription readScene(FileIO::FileIO& io, std::string_view path)
    {
        const auto native = filePath(path);
        try
        {
            auto f = io.open(native, FileIO::OpenMode::Read);
            if (!f)
                fail("file", "cannot open");
            const auto size = f->size();
            if (size > std::string{}.max_size())
                fail("file", "too large");
            std::string data(static_cast<size_t>(size), '\0');
            size_t pos = 0;
            while (pos < data.size())
            {
                auto n = f->read(std::span(reinterpret_cast<uint8_t*>(data.data() + pos), data.size() - pos));
                if (!n)
                    fail("file", "short read");
                pos += n;
            }
            return decodeScene(data);
        }
        catch (const Error::Exception& e)
        {
            throw Error::Exception(Error::Code::IO, fmt::format("Scene file '{}': {}", path, e.message()));
        }
    }
    void writeScene(FileIO::FileIO& io, std::string_view path, const SceneDescription& d)
    {
        const auto data = encodeScene(d);
        const auto native = filePath(path);
        try
        {
            auto f = io.open(native, FileIO::OpenMode::Write | FileIO::OpenMode::Create | FileIO::OpenMode::Truncate);
            if (!f)
                fail("file", "cannot open");
            size_t pos = 0;
            while (pos < data.size())
            {
                auto n = f->write(std::span(reinterpret_cast<const uint8_t*>(data.data() + pos), data.size() - pos));
                if (!n)
                    fail("file", "short write");
                pos += n;
            }
            f->flush();
        }
        catch (const Error::Exception& e)
        {
            throw Error::Exception(Error::Code::IO, fmt::format("Scene file '{}': {}", path, e.message()));
        }
    }
}  // namespace Incubator::Scene
