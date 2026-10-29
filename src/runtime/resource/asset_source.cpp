#include "runtime/resource/asset_source.h"

#include <algorithm>

#include "runtime/core/error/error.h"
#include "runtime/core/path/path.h"
#include "runtime/core/utils/utf8.h"

namespace Incubator::Resource
{
    AssetSource::AssetSource(FileIO::FileIO& fileIO, const std::filesystem::path& root)
        : fileIO_(fileIO), root_(std::filesystem::absolute(root).lexically_normal())
    {}

    std::filesystem::path AssetSource::resolve(std::string_view virtualPath) const
    {
        if (virtualPath.empty() || !Utils::Utf8::isValid(virtualPath) || virtualPath.front() == '/' ||
            virtualPath.front() == '\\' || virtualPath.find(':') != std::string_view::npos ||
            virtualPath.find('\0') != std::string_view::npos)
            throw Error::Exception(Error::Code::IO, "Asset path must be a relative UTF-8 file path");
        const auto normalized = Path::normalize(virtualPath);
        if (normalized == "." || normalized == ".." || normalized.starts_with("../"))
            throw Error::Exception(Error::Code::IO, "Asset path escapes root or does not name a file");
        return std::filesystem::path((root_ / Utils::Utf8::toFsPath(normalized)).generic_u8string());
    }

    std::vector<AssetEntry> AssetSource::listDirectory(std::string_view directory) const
    {
        std::string key;
        if (!directory.empty() && directory != ".")
        {
            resolve(directory);
            key = Path::normalize(directory);
        }
        auto physical = root_;
        // 对每级目录检查直接子项，避免通过 link/child 绕过目录重解析点过滤。
        for (const auto& part : Utils::Utf8::toFsPath(key))
        {
            const auto name = Utils::Utf8::toUtf8String(part);
            const auto entries = fileIO_.listDirectory(physical);
            const auto found = std::ranges::find_if(
                entries, [&](const auto& entry) { return Path::toLowerASCII(entry.name) == Path::toLowerASCII(name); });
            if (found == entries.end() || !found->directory || found->reparsePoint)
                throw Error::Exception(Error::Code::IO,
                                       "Asset directory is missing or is a link: " + std::string(directory));
            physical /= part;
        }
        std::vector<AssetEntry> result;
        for (const auto& entry : fileIO_.listDirectory(physical))
        {
            if (entry.directory && entry.reparsePoint)
                continue;
            const auto path = key.empty() ? entry.name : Path::join(key, entry.name);
            auto kind = AssetKind::File;
            if (entry.directory)
                kind = AssetKind::Directory;
            else if (Path::hasExtension(path, "gltf") || Path::hasExtension(path, "obj"))
                kind = AssetKind::Model;
            else if (Path::hasExtension(path, "png"))
                kind = AssetKind::Texture;
            else if (Path::hasExtension(path, "bin") || Path::hasExtension(path, "mtl"))
                kind = AssetKind::Dependency;
            result.push_back({entry.name, Path::toLowerASCII(path), kind});
        }
        std::ranges::sort(result, {}, &AssetEntry::path);
        return result;
    }

    bool AssetSource::exists(std::string_view virtualPath) const
    {
        return fileIO_.exists(resolve(virtualPath));
    }

    std::unique_ptr<FileIO::FileHandle> AssetSource::open(std::string_view virtualPath, FileIO::ShareMode share) const
    {
        return fileIO_.open(resolve(virtualPath), FileIO::OpenMode::Read, share);
    }
}  // namespace Incubator::Resource
