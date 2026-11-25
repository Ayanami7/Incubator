#include "runtime/resource/loader/detail/model_source.h"
#include "runtime/resource/loader/detail/image_decode.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <array>
#include <cmath>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/core/path/path.h"
#include "runtime/platform/fileio/file_handle.h"
#include "runtime/resource/resource_id.h"

namespace Incubator::Resource::Detail
{
    namespace
    {
        const rapidjson::Value* member(const rapidjson::Value& object, const char* name)
        {
            if (!object.IsObject())
            {
                throw Error::Exception(Error::Code::IO, "Invalid glTF object");
            }
            auto found = object.FindMember(name);
            return found == object.MemberEnd() ? nullptr : &found->value;
        }

        const rapidjson::Value* arrayMember(const rapidjson::Value& object, const char* name)
        {
            const auto* value = member(object, name);
            if (value && !value->IsArray())
            {
                throw Error::Exception(Error::Code::IO, fmt::format("Invalid glTF {} array", name));
            }
            return value;
        }

        void checkEmbeddedDependencies(const rapidjson::Value& source, uint32_t binLength)
        {
            uint32_t bufferLength = 0;
            if (const auto* buffers = arrayMember(source, "buffers"); buffers && !buffers->Empty())
            {
                if (buffers->Size() != 1 || member((*buffers)[0], "uri"))
                    throw Error::Exception(Error::Code::IO, "GLB requires a single embedded buffer without URI");
                const auto* length = member((*buffers)[0], "byteLength");
                if (!length || !length->IsUint() || length->GetUint() == 0 || length->GetUint() > binLength)
                    throw Error::Exception(Error::Code::IO, "GLB buffer byteLength exceeds or lacks the BIN chunk");
                bufferLength = length->GetUint();
            }

            const auto* views = arrayMember(source, "bufferViews");
            if (views)
            {
                for (const auto& view : views->GetArray())
                {
                    const auto* buffer = member(view, "buffer");
                    const auto* offset = member(view, "byteOffset");
                    const auto* length = member(view, "byteLength");
                    if (!buffer || !buffer->IsUint() || buffer->GetUint() != 0 ||
                        (offset && !offset->IsUint()) || !length || !length->IsUint() || length->GetUint() == 0)
                        throw Error::Exception(Error::Code::IO, "Invalid GLB bufferView byte range or buffer index");
                    const auto start = offset ? offset->GetUint() : 0;
                    if (start > bufferLength || length->GetUint() > bufferLength - start)
                        throw Error::Exception(Error::Code::IO, "GLB bufferView exceeds the BIN buffer");
                }
            }
            if (const auto* images = arrayMember(source, "images"))
            {
                for (const auto& image : images->GetArray())
                {
                    if (member(image, "uri"))
                        throw Error::Exception(Error::Code::IO, "GLB images must be embedded without URI");
                    const auto* view = member(image, "bufferView");
                    if (!view || !view->IsUint() || !views || view->GetUint() >= views->Size())
                        throw Error::Exception(Error::Code::IO, "Invalid GLB image bufferView index");
                    const auto* mime = member(image, "mimeType");
                    const auto type = mime && mime->IsString()
                                          ? std::string_view(mime->GetString(), mime->GetStringLength())
                                          : std::string_view{};
                    if (type != "image/png" && type != "image/jpeg")
                        throw Error::Exception(Error::Code::IO, "GLB image mimeType must be image/png or image/jpeg");
                }
            }
        }

        void checkGltf(std::string_view path, std::string_view text, bool isGlb = false, uint32_t binLength = 0)
        {
            rapidjson::Document source;
            source.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(), text.size());
            if (source.HasParseError())
            {
                throw Error::Exception(
                    Error::Code::IO,
                    fmt::format("Invalid glTF JSON: {}", rapidjson::GetParseError_En(source.GetParseError())));
            }
            const auto* asset = member(source, "asset");
            const auto* version = asset ? member(*asset, "version") : nullptr;
            if (!version || !version->IsString() || std::string_view(version->GetString()) != "2.0")
            {
                throw Error::Exception(Error::Code::IO, "Only glTF 2.0 is supported");
            }
            for (const char* name : {"animations", "skins"})
            {
                const auto* items = arrayMember(source, name);
                if (items && !items->Empty())
                {
                    throw Error::Exception(Error::Code::IO, fmt::format("Unsupported glTF {}", name));
                }
            }
            if (const auto* nodes = arrayMember(source, "nodes"))
            {
                // 导入器会递归复制节点，因此必须在运行前拒绝环和多重父节点。
                std::vector<bool> hasParent(nodes->Size(), false);
                for (const auto& node : nodes->GetArray())
                {
                    if (const auto* children = arrayMember(node, "children"))
                    {
                        for (const auto& child : children->GetArray())
                        {
                            if (!child.IsUint() || child.GetUint() >= nodes->Size() || hasParent[child.GetUint()])
                            {
                                throw Error::Exception(Error::Code::IO,
                                                       "Invalid glTF hierarchy child reference or multiple parents");
                            }
                            hasParent[child.GetUint()] = true;
                        }
                    }
                }
                std::vector<rapidjson::SizeType> pending;
                for (rapidjson::SizeType i = 0; i < nodes->Size(); ++i)
                {
                    if (!hasParent[i])
                        pending.push_back(i);
                }
                size_t reached = 0;
                while (!pending.empty())
                {
                    const auto index = pending.back();
                    pending.pop_back();
                    ++reached;
                    if (const auto* children = arrayMember((*nodes)[index], "children"))
                    {
                        for (const auto& child : children->GetArray())
                            pending.push_back(child.GetUint());
                    }
                }
                if (reached != nodes->Size())
                    throw Error::Exception(Error::Code::IO, "Invalid glTF hierarchy cycle");
            }
            if (isGlb)
            {
                checkEmbeddedDependencies(source, binLength);
            }
            else
            {
                const auto directory = Path::getParentPath(path);
                for (const char* name : {"buffers", "images"})
                {
                    const auto* items = arrayMember(source, name);
                    if (!items)
                        continue;
                    for (const auto& item : items->GetArray())
                    {
                        const auto* uri = member(item, "uri");
                        if (!uri || !uri->IsString())
                        {
                            throw Error::Exception(Error::Code::IO, "Embedded model buffers/images are unsupported");
                        }
                        const auto key = modelDependencyPath(
                            directory, std::string_view(uri->GetString(), uri->GetStringLength()), true);
                        if (std::string_view(name) == "images" && !imageFormatFromPath(key))
                        {
                            throw Error::Exception(Error::Code::IO,
                                                   "Only external PNG or JPEG model images are supported");
                        }
                    }
                }
            }
            if (const auto* meshes = arrayMember(source, "meshes"))
            {
                for (const auto& mesh : meshes->GetArray())
                {
                    if (const auto* primitives = arrayMember(mesh, "primitives"))
                    {
                        for (const auto& primitive : primitives->GetArray())
                        {
                            const auto* attributes = member(primitive, "attributes");
                            const auto* position = attributes ? member(*attributes, "POSITION") : nullptr;
                            const auto* accessors = arrayMember(source, "accessors");
                            if (position && position->IsUint() && accessors && position->GetUint() < accessors->Size())
                            {
                                const auto* count = member((*accessors)[position->GetUint()], "count");
                                // Assimp 使用 unsigned 保存 aiMesh::mNumVertices；拒绝超出其表示范围的源顶点数。
                                if (count && count->IsUint64() &&
                                    count->GetUint64() > std::numeric_limits<unsigned>::max())
                                {
                                    throw Error::Exception(Error::Code::IO,
                                                           "Model source geometry exceeds Assimp vertex count range");
                                }
                            }
                            const auto* targets = arrayMember(primitive, "targets");
                            const auto* extensions = member(primitive, "extensions");
                            if ((targets && !targets->Empty()) ||
                                (extensions && member(*extensions, "KHR_draco_mesh_compression")))
                            {
                                throw Error::Exception(Error::Code::IO,
                                                       "Morph/compressed model geometry is unsupported");
                            }
                        }
                    }
                }
            }
            if (const auto* views = arrayMember(source, "bufferViews"))
            {
                for (const auto& view : views->GetArray())
                {
                    const auto* extensions = member(view, "extensions");
                    if (extensions && member(*extensions, "EXT_meshopt_compression"))
                    {
                        throw Error::Exception(Error::Code::IO, "Compressed model geometry is unsupported");
                    }
                }
            }
            if (const auto* materials = arrayMember(source, "materials"))
            {
                const auto inUnitRange = [](const rapidjson::Value& value) {
                    if (!value.IsNumber())
                        return false;
                    const auto number = value.GetDouble();
                    return std::isfinite(number) && number >= 0.0 && number <= 1.0;
                };
                for (rapidjson::SizeType i = 0; i < materials->Size(); ++i)
                {
                    const auto checkTextureSlot = [i](const rapidjson::Value* texture, std::string_view label) {
                        if (!texture)
                            return;
                        const auto* uv = member(*texture, "texCoord");
                        const auto* extensions = member(*texture, "extensions");
                        if (uv && (!uv->IsUint() || uv->GetUint() != 0))
                        {
                            throw Error::Exception(Error::Code::IO,
                                                   fmt::format("Model material {} requires {} UV0", i, label));
                        }
                        if (extensions && member(*extensions, "KHR_texture_transform"))
                        {
                            throw Error::Exception(
                                Error::Code::IO,
                                fmt::format("Model material {} has unsupported {} UV transform", i, label));
                        }
                    };
                    const auto& mat = (*materials)[i];
                    if (const auto* alphaMode = member(mat, "alphaMode"))
                    {
                        const auto mode = alphaMode->IsString()
                                              ? std::string_view(alphaMode->GetString(), alphaMode->GetStringLength())
                                              : std::string_view{};
                        if (mode != "OPAQUE" && mode != "MASK" && mode != "BLEND")
                            throw Error::Exception(Error::Code::IO, fmt::format("Model material {} invalid alphaMode", i));
                    }
                    if (const auto* cutoff = member(mat, "alphaCutoff"))
                    {
                        const auto value = cutoff->IsNumber() ? cutoff->GetDouble() : -1.0;
                        if (!std::isfinite(value) || value < 0.0 || value > std::numeric_limits<float>::max())
                            throw Error::Exception(Error::Code::IO, fmt::format("Model material {} invalid alphaCutoff", i));
                    }
                    if (const auto* sides = member(mat, "doubleSided"); sides && !sides->IsBool())
                        throw Error::Exception(Error::Code::IO, fmt::format("Model material {} invalid doubleSided", i));
                    if (const auto* factor = arrayMember(mat, "emissiveFactor"))
                    {
                        if (factor->Size() != 3 || !inUnitRange((*factor)[0]) || !inUnitRange((*factor)[1]) ||
                            !inUnitRange((*factor)[2]))
                            throw Error::Exception(Error::Code::IO,
                                                   fmt::format("Model material {} invalid emissiveFactor", i));
                    }
                    checkTextureSlot(member(mat, "emissiveTexture"), "emissive");
                    const auto* pbr = member(mat, "pbrMetallicRoughness");
                    checkTextureSlot(pbr ? member(*pbr, "baseColorTexture") : nullptr, "base color");
                    checkTextureSlot(pbr ? member(*pbr, "metallicRoughnessTexture") : nullptr, "metallic roughness");
                    const auto* normalTex = member(mat, "normalTexture");
                    checkTextureSlot(normalTex, "normal");

                    if (pbr)
                    {
                        const auto* color = arrayMember(*pbr, "baseColorFactor");
                        if (color && color->Size() > 3 && !inUnitRange((*color)[3]))
                            throw Error::Exception(Error::Code::IO,
                                                   fmt::format("Model material {} invalid baseColorFactor alpha", i));
                        if (const auto* metallic = member(*pbr, "metallicFactor"))
                        {
                            const double value = metallic->IsNumber() ? metallic->GetDouble()
                                                                      : std::numeric_limits<double>::quiet_NaN();
                            if (!std::isfinite(value) || value < 0.0 || value > 1.0)
                            {
                                throw Error::Exception(
                                    Error::Code::IO,
                                    fmt::format("Model material {} requires finite metallicFactor in [0, 1]", i));
                            }
                        }
                        if (const auto* roughness = member(*pbr, "roughnessFactor"))
                        {
                            const double value = roughness->IsNumber() ? roughness->GetDouble()
                                                                       : std::numeric_limits<double>::quiet_NaN();
                            if (!std::isfinite(value) || value < 0.0 || value > 1.0)
                            {
                                throw Error::Exception(
                                    Error::Code::IO,
                                    fmt::format("Model material {} requires finite roughnessFactor in [0, 1]", i));
                            }
                        }
                    }
                    if (normalTex)
                    {
                        if (const auto* scale = member(*normalTex, "scale"))
                        {
                            const double value = scale->IsNumber() ? scale->GetDouble()
                                                                   : std::numeric_limits<double>::quiet_NaN();
                            if (!std::isfinite(value) || value < 0.0)
                            {
                                throw Error::Exception(
                                    Error::Code::IO,
                                    fmt::format("Model material {} requires finite nonnegative normalTexture.scale", i));
                            }
                        }
                    }
                }
            }
            if (const auto* extensions = arrayMember(source, "extensionsRequired"))
            {
                for (const auto& extension : extensions->GetArray())
                {
                    if (!extension.IsString() || std::string_view(extension.GetString()) != "KHR_materials_unlit")
                    {
                        throw Error::Exception(Error::Code::IO, "Unsupported required glTF extension");
                    }
                }
            }
        }

        void checkMtl(std::string_view text, std::unordered_set<std::string>& diffuseMaterials)
        {
            // Assimp 会丢弃 map_Kd 的 UV 变换和重复的基础纹理声明。
            // 只检查这些声明；颜色、不透明度和纹理文件名仍交由 Assimp 处理。
            std::istringstream lines{std::string(text)};
            std::string line;
            std::string material = "default";
            std::unordered_set<std::string> textured;
            while (std::getline(lines, line))
            {
                std::istringstream words(line.substr(0, line.find('#')));
                std::string keyword;
                words >> keyword;
                keyword = Path::toLowerASCII(keyword);
                if (keyword == "newmtl")
                {
                    std::getline(words >> std::ws, material);
                    const auto end = material.find_last_not_of(" \t\r");
                    material.erase(end == std::string::npos ? 0 : end + 1);
                }
                else if (keyword == "map_kd")
                {
                    if (!textured.insert(material).second)
                    {
                        throw Error::Exception(
                            Error::Code::IO,
                            fmt::format("Model material '{}' has multiple base color textures", material));
                    }
                    std::string option;
                    while (words >> option)
                    {
                        if (option.starts_with('/') || option.starts_with('\\') ||
                            option.find(':') != std::string::npos)
                        {
                            throw Error::Exception(
                                Error::Code::IO,
                                fmt::format("Model material '{}' has unsupported texture dependency", material));
                        }
                        const auto normalizedOption = Path::toLowerASCII(option);
                        if (normalizedOption == "-o" || normalizedOption == "-s" || normalizedOption == "-t" ||
                            normalizedOption == "-type")
                        {
                            throw Error::Exception(
                                Error::Code::IO,
                                fmt::format("Model material '{}' has unsupported base color UV transform/mapping",
                                            material));
                        }
                    }
                }
                else if (keyword == "kd")
                {
                    // 否则 Assimp 会插入灰色（0.6），导致无法识别源文件未提供 Kd。
                    diffuseMaterials.insert(material);
                }
            }
        }
    }  // namespace

    void checkGlb(FileIO::FileHandle& file)
    {
        constexpr uint64_t MAX_FILE_SIZE = 256u * 1024u * 1024u;
        const auto size = file.size();
        if (size < 20 || size > MAX_FILE_SIZE)
            throw Error::Exception(Error::Code::IO, "Invalid GLB file size (limit 256 MiB)");

        const auto read = [&](std::span<uint8_t> bytes) {
            while (!bytes.empty())
            {
                const auto count = file.read(bytes);
                if (!count)
                    throw Error::Exception(Error::Code::IO, "Incomplete GLB header or JSON read");
                bytes = bytes.subspan(count);
            }
        };
        const auto word = [](const uint8_t* bytes) {
            return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8u) | (uint32_t(bytes[2]) << 16u) |
                   (uint32_t(bytes[3]) << 24u);
        };
        std::array<uint8_t, 12> header{};
        read(header);
        if (word(header.data()) != 0x46546c67u)
            throw Error::Exception(Error::Code::IO, "Invalid GLB magic");
        if (word(header.data() + 4) != 2)
            throw Error::Exception(Error::Code::IO, "Only GLB version 2 is supported");
        if (word(header.data() + 8) != size)
            throw Error::Exception(Error::Code::IO, "GLB declared length does not match file size");

        std::array<uint8_t, 8> chunk{};
        read(chunk);
        const auto jsonLength = word(chunk.data());
        if (word(chunk.data() + 4) != 0x4e4f534au || !jsonLength || jsonLength % 4 || jsonLength > size - 20)
            throw Error::Exception(Error::Code::IO, "Invalid GLB JSON chunk type or length");
        std::string json(jsonLength, '\0');
        read({reinterpret_cast<uint8_t*>(json.data()), json.size()});
        if (json.find('\0') != std::string::npos)
            throw Error::Exception(Error::Code::IO, "Invalid NUL in GLB JSON chunk");

        const auto remaining = size - 20 - jsonLength;
        uint32_t binLength = 0;
        if (remaining != 0)
        {
            if (remaining < chunk.size())
                throw Error::Exception(Error::Code::IO, "Truncated GLB BIN chunk header");
            read(chunk);
            binLength = word(chunk.data());
            if (word(chunk.data() + 4) != 0x004e4942u || binLength % 4 ||
                binLength != remaining - chunk.size())
                throw Error::Exception(Error::Code::IO, "Invalid GLB BIN chunk type or length");
        }
        checkGltf({}, json, true, binLength);
    }

    std::string decodeModelUri(std::string_view path)
    {
        std::string result;
        result.reserve(path.size());
        const auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < path.size(); ++i)
        {
            if (path[i] == '%')
            {
                if (i + 2 >= path.size() || hex(path[i + 1]) < 0 || hex(path[i + 2]) < 0)
                {
                    throw Error::Exception(Error::Code::IO, "Invalid model URI escape");
                }
                result.push_back(static_cast<char>(hex(path[i + 1]) * 16 + hex(path[i + 2])));
                i += 2;
            }
            else
                result.push_back(path[i]);
        }
        return result;
    }

    std::string modelDependencyPath(std::string_view directory, std::string_view path, bool uriEncoded)
    {
        const auto decoded = uriEncoded ? decodeModelUri(path) : std::string(path);
        if (decoded.empty() || decoded[0] == '/' || decoded[0] == '\\' || decoded.find(':') != std::string::npos ||
            decoded.find('\0') != std::string::npos)
        {
            throw Error::Exception(Error::Code::IO, fmt::format("Unsupported model dependency '{}'", path));
        }
        auto key = canonicalizeResourceKey(Path::join(directory, decoded));
        if (key.empty() || key == ".")
        {
            throw Error::Exception(Error::Code::IO, fmt::format("Model dependency '{}' escapes assets root", path));
        }
        return key;
    }

    void checkModelSource(std::string_view path, std::string_view text,
                          std::unordered_set<std::string>& objDiffuseMaterials)
    {
        if (Path::hasExtension(path, "gltf"))
            checkGltf(path, text);
        else if (Path::hasExtension(path, "mtl"))
            checkMtl(text, objDiffuseMaterials);
    }
}  // namespace Incubator::Resource::Detail
