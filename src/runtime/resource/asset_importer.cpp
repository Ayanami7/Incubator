#include "runtime/resource/asset_importer.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <algorithm>
#include <array>
#include <exception>
#include <string_view>
#include <unordered_set>

#include "runtime/core/error/error.h"
#include "runtime/core/path/path.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/resource/asset_source.h"
#include "runtime/resource/loader/detail/image_decode.h"
#include "runtime/resource/loader/detail/model_source.h"

namespace Incubator::Resource
{
    namespace
    {
        namespace Fs = std::filesystem;
        using Utils::Utf8::toFsPath;
        using Utils::Utf8::toUtf8String;

        [[noreturn]] void fail(const Fs::path& path, std::string_view reason)
        {
            throw Error::Exception(Error::Code::IO, std::string(reason) + ": " + toUtf8String(path));
        }

        void checkComponents(std::string_view path)
        {
            if (!Utils::Utf8::isValid(path) || path.find('\0') != std::string_view::npos)
                fail({}, "Import path must be valid UTF-8 without NUL");
            for (const auto& part : toFsPath(path))
            {
                const auto name = toUtf8String(part);
                if (name == "." || name == ".." || name.empty() || name == "/" || name == "\\")
                    continue;
                if (name.back() == '.' || name.back() == ' ' || name.find_first_of("<>:\"|?*") != name.npos ||
                    std::ranges::any_of(name, [](unsigned char c) { return c < 32; }))
                    fail(part, "Invalid import path component");
            }
        }

        void createDirectory(FileIO::FileIO& io, const Fs::path& path, std::vector<Fs::path>& created)
        {
            created.push_back(path);
            if (!io.createDirectory(path))
            {
                created.pop_back();
                fail(path, "Cannot create import directory");
            }
        }

        void checkDirectories(FileIO::FileIO& io, const Fs::path& path, std::vector<Fs::path>* created = nullptr)
        {
            auto current = path.root_path();
            for (const auto& part : path.relative_path())
            {
                if (part.empty())
                    continue;
                current /= part;
                const auto entry = io.inspect(current);
                if (!entry && created)
                    createDirectory(io, current, *created);
                else if (!entry || !entry->directory || entry->reparsePoint)
                    fail(current, "Import directory is missing, is not a directory, or is a reparse point");
            }
        }

        std::unique_ptr<FileIO::FileHandle> openInput(FileIO::FileIO& io, const Fs::path& path)
        {
            auto file = io.open(path, FileIO::OpenMode::Read, FileIO::ShareMode::Read);
            if (!file)
                fail(path, "Cannot open import source");
            return file;
        }

        void checkInput(FileIO::FileIO& io, const Fs::path& path)
        {
            checkDirectories(io, path.parent_path());
            const auto entry = io.inspect(path);
            if (!entry || entry->directory || entry->reparsePoint)
                fail(path, "Import source is missing or is not a regular file");
        }

        void readExactly(FileIO::FileHandle& file, std::span<uint8_t> bytes, const Fs::path& path)
        {
            while (!bytes.empty())
            {
                const auto count = file.read(bytes);
                if (!count)
                    fail(path, "Import source ended before its reported size");
                bytes = bytes.subspan(count);
            }
        }

        std::string readText(FileIO::FileIO& io, const Fs::path& path)
        {
            auto file = openInput(io, path);
            const auto size = file->size();
            if (size > std::string{}.max_size())
                fail(path, "Import source text is too large");
            std::string text(static_cast<size_t>(size), '\0');
            readExactly(*file, {reinterpret_cast<uint8_t*>(text.data()), text.size()}, path);
            return text;
        }

        std::string dependency(std::string_view directory, std::string_view reference, bool uriEncoded)
        {
            const auto decoded = uriEncoded ? Detail::decodeModelUri(reference) : std::string(reference);
            checkComponents(decoded);
            Detail::modelDependencyPath(directory, decoded, false);
            return Path::normalize(Path::join(directory, decoded));
        }

        void checkImageDependency(std::string_view path)
        {
            if (!Detail::imageFormatFromPath(path))
                fail(toFsPath(path), "Unsupported import image extension (expected PNG or JPEG)");
        }

        template <class Callback>
        void gltfReferences(std::string_view source, const Fs::path& path, Callback add)
        {
            rapidjson::Document document;
            document.Parse<rapidjson::kParseValidateEncodingFlag>(source.data(), source.size());
            if (document.HasParseError())
                fail(path, rapidjson::GetParseError_En(document.GetParseError()));
            if (!document.IsObject())
                fail(path, "Expected a glTF JSON object");

            const auto collect = [&](const char* field, bool image) {
                const auto array = document.FindMember(field);
                if (array == document.MemberEnd())
                    return;
                if (!array->value.IsArray())
                    fail(path, std::string("Expected a glTF array for ") + field);
                for (const auto& item : array->value.GetArray())
                {
                    if (!item.IsObject() || !item.HasMember("uri") || !item["uri"].IsString())
                        fail(path, std::string("Expected an external glTF URI in ") + field);
                    const auto& uri = item["uri"];
                    auto relative = dependency({}, {uri.GetString(), uri.GetStringLength()}, true);
                    if (image)
                        checkImageDependency(relative);
                    add(std::move(relative));
                }
            };
            collect("buffers", false);
            collect("images", true);
        }

        std::string_view trimLeft(std::string_view text)
        {
            const auto start = text.find_first_not_of(" \t");
            return start == text.npos ? std::string_view{} : text.substr(start);
        }

        std::string_view nextWord(std::string_view& text)
        {
            text = trimLeft(text);
            const auto end = text.find_first_of(" \t");
            const auto word = text.substr(0, end);
            text.remove_prefix(word.size());
            return word;
        }

        template <class Callback>
        void textReferences(std::string_view text, std::string_view keyword, Callback add)
        {
            if (text.starts_with("\xef\xbb\xbf"))
                text.remove_prefix(3);
            while (!text.empty())
            {
                const auto end = text.find_first_of("\r\n");
                auto line = text.substr(0, end);
                text.remove_prefix(line.size() + (end != text.npos));
                const auto word = nextWord(line);
                if (word == keyword || (keyword == "map_kd" && Path::toLowerASCII(word) == keyword))
                    add(trimLeft(line));
            }
        }

        std::string_view diffuseTexturePath(std::string_view text, const Fs::path& path)
        {
            // 按固定版本 Assimp 的参数个数跳过选项，剩余整段是文件名，可包含空格。
            while (text.starts_with('-'))
            {
                const auto option = Path::toLowerASCII(nextWord(text));
                int arguments = 0;
                if (option == "-mm")
                    arguments = 2;
                else if (option == "-clamp" || option == "-bm" || option == "-blendu" || option == "-blendv" ||
                         option == "-boost" || option == "-texres" || option == "-imfchan")
                    arguments = 1;
                for (int i = 0; i < arguments; ++i)
                {
                    if (nextWord(text).empty())
                        fail(path, "Missing map_Kd option argument");
                }
                text = trimLeft(text);
            }
            const auto end = text.find_last_not_of(" \t");
            return end == text.npos ? std::string_view{} : text.substr(0, end + 1);
        }

        void copyFile(FileIO::FileIO& io, const Fs::path& source, const Fs::path& target)
        {
            using FileIO::OpenMode;
            auto input = openInput(io, source);
            auto output = io.open(target, OpenMode::Write | OpenMode::Create | OpenMode::Truncate);
            if (!output)
                fail(target, "Cannot open import destination");
            std::array<uint8_t, 64 * 1024> buffer;
            for (auto remaining = input->size(); remaining != 0;)
            {
                const auto size = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
                auto bytes = std::span(buffer).first(size);
                readExactly(*input, bytes, source);
                while (!bytes.empty())
                {
                    const auto count = output->write(bytes);
                    if (!count)
                        fail(target, "Cannot write import destination");
                    bytes = bytes.subspan(count);
                }
                remaining -= size;
            }
            output->flush();
        }
    }  // namespace

    ImportedModelResult importExternalModel(AssetSource& targetSource, const Fs::path& externalSourcePath,
                                            const ModelImportOptions& options)
    {
        auto& io = targetSource.fileIO();
        std::error_code error;
        const auto source = Fs::absolute(externalSourcePath, error).lexically_normal();
        if (error)
            fail(externalSourcePath, error.message());
        const auto filename = toUtf8String(source.filename());
        const auto stem = toUtf8String(source.stem());
        const bool gltf = Path::hasExtension(filename, "gltf");
        const bool glb = Path::hasExtension(filename, "glb");
        if (!gltf && !glb && !Path::hasExtension(filename, "obj"))
            fail(source, "Only external glTF, GLB and OBJ models can be imported");
        checkComponents(filename);
        checkComponents(stem);
        checkInput(io, source);

        const auto root = source.parent_path();
        std::vector<std::string> files{filename};
        std::unordered_set<std::string> seen{Path::toLowerASCII(filename)};
        const auto add = [&](std::string relative) {
            if (!seen.insert(Path::toLowerASCII(relative)).second)
                return false;
            const auto path = root / toFsPath(relative);
            checkInput(io, path);
            openInput(io, path);
            files.push_back(std::move(relative));
            return true;
        };
        if (glb)
        {
            try
            {
                Detail::checkGlb(*openInput(io, source));
            }
            catch (const Error::Exception& exception)
            {
                fail(source, exception.message());
            }
        }
        else if (gltf)
            gltfReferences(readText(io, source), source, add);
        else
        {
            textReferences(readText(io, source), "mtllib", [&](std::string_view reference) {
                const auto material = dependency({}, reference, false);
                if (!Path::hasExtension(material, "mtl"))
                    fail(toFsPath(material), "Unsupported import dependency extension");
                if (!add(material))
                    return;
                const auto path = root / toFsPath(material);
                const auto materialText = readText(io, path);
                std::unordered_set<std::string> diffuseMaterials;
                Detail::checkModelSource(material, materialText, diffuseMaterials);
                textReferences(materialText, "map_kd", [&](std::string_view texture) {
                    auto relative = dependency(Path::getParentPath(material), diffuseTexturePath(texture, path), false);
                    checkImageDependency(relative);
                    add(std::move(relative));
                });
            });
        }

        const auto parent = options.targetDirectory.empty() ? targetSource.root()
                                                             : targetSource.resolve(options.targetDirectory);
        checkComponents(options.targetDirectory);
        const auto virtualParent = Path::normalize(options.targetDirectory);
        std::vector<Fs::path> createdDirectories;
        std::vector<Fs::path> createdFiles;
        try
        {
            checkDirectories(io, parent, &createdDirectories);
            auto name = stem;
            for (size_t suffix = 1; io.inspect(parent / toFsPath(name)); ++suffix)
                name = stem + "_" + std::to_string(suffix);

            const auto destination = parent / toFsPath(name);
            createDirectory(io, destination, createdDirectories);
            ImportedModelResult result;
            result.targetDirectory = Path::toLowerASCII(Path::join(virtualParent, name));
            for (const auto& relative : files)
            {
                const auto target = destination / toFsPath(relative);
                checkDirectories(io, target.parent_path(), &createdDirectories);
                createdFiles.push_back(target);
                copyFile(io, root / toFsPath(relative), target);
                result.copiedVirtualPaths.push_back(Path::toLowerASCII(Path::join(result.targetDirectory, relative)));
            }
            result.virtualPath = result.copiedVirtualPaths.front();
            return result;
        }
        catch (...)
        {
            // 先删除文件，再按创建顺序的逆序删除目录。
            bool cleaned = true;
            for (auto it = createdFiles.rbegin(); it != createdFiles.rend(); ++it)
                cleaned &= io.remove(*it) || !io.exists(*it);
            for (auto it = createdDirectories.rbegin(); it != createdDirectories.rend(); ++it)
                cleaned &= io.removeDirectory(*it);
            if (!cleaned)
                std::throw_with_nested(Error::Exception(Error::Code::IO, "Cannot completely roll back model import"));
            throw;
        }
    }
}  // namespace Incubator::Resource
