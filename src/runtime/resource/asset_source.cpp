#include "runtime/resource/asset_source.h"

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

    bool AssetSource::exists(std::string_view virtualPath) const
    {
        return fileIO_.exists(resolve(virtualPath));
    }

    std::unique_ptr<FileIO::FileHandle> AssetSource::open(std::string_view virtualPath, FileIO::ShareMode share) const
    {
        return fileIO_.open(resolve(virtualPath), FileIO::OpenMode::Read, share);
    }
}  // namespace Incubator::Resource
