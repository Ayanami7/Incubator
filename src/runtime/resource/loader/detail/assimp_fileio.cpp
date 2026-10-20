#include "runtime/resource/loader/detail/assimp_fileio.h"

#include <algorithm>
#include <bit>
#include <exception>
#include <limits>
#include <span>
#include <utility>

#include "runtime/core/error/error.h"
#include "runtime/core/path/path.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/resource/loader/detail/model_source.h"

namespace Incubator::Resource::Detail
{
    void ImportIoState::fail(std::string_view path, std::string_view reason)
    {
        if (failure.empty())
        {
            failure = fmt::format("{}: {}", path, reason);
        }
    }

    AssimpFileStream::AssimpFileStream(std::unique_ptr<FileIO::FileHandle> file, std::string path, ImportIoState& state)
        : file_(std::move(file)), path_(std::move(path)), state_(state), size_(0)
    {
        const uint64_t size = file_->size();
        if (size > std::numeric_limits<size_t>::max() || size > std::numeric_limits<int64_t>::max())
        {
            throw Error::Exception(Error::Code::IO, "Model input exceeds supported file offset range");
        }
        size_ = static_cast<size_t>(size);
    }

    size_t AssimpFileStream::Read(void* buffer, size_t size, size_t count)
    {
        if (size == 0 || count == 0)
        {
            return 0;
        }
        if (count > std::numeric_limits<size_t>::max() / size)
        {
            state_.fail(path_, "Model read size overflow");
            return 0;
        }
        try
        {
            const size_t position = static_cast<size_t>(file_->tell());
            const size_t bytes = std::min(size * count, size_ - position);
            auto output = std::span(static_cast<uint8_t*>(buffer), bytes);
            size_t read = 0;
            while (read < bytes)
            {
                const size_t chunk = file_->read(output.subspan(read));
                if (chunk == 0)
                {
                    state_.fail(path_, "Incomplete model read before reported EOF");
                    break;
                }
                read += chunk;
            }
            return read / size;
        }
        catch (const std::exception& error)
        {
            state_.fail(path_, error.what());
            return 0;
        }
    }

    size_t AssimpFileStream::Write(const void*, size_t, size_t)
    {
        state_.fail(path_, "Model input is read-only");
        return 0;
    }

    aiReturn AssimpFileStream::Seek(size_t offset, aiOrigin origin)
    {
        try
        {
            int64_t base = 0;
            FileIO::SeekOrigin fileOrigin;
            switch (origin)
            {
                case aiOrigin_SET:
                    fileOrigin = FileIO::SeekOrigin::Begin;
                    break;
                case aiOrigin_CUR:
                    base = static_cast<int64_t>(file_->tell());
                    fileOrigin = FileIO::SeekOrigin::Current;
                    break;
                case aiOrigin_END:
                    base = static_cast<int64_t>(size_);
                    fileOrigin = FileIO::SeekOrigin::End;
                    break;
                default:
                    state_.fail(path_, "Invalid model seek origin");
                    return aiReturn_FAILURE;
            }
            const int64_t delta = std::bit_cast<int64_t>(static_cast<uint64_t>(offset));
            if (delta < -base || delta > static_cast<int64_t>(size_) - base)
            {
                state_.fail(path_, "Model seek outside file bounds");
                return aiReturn_FAILURE;
            }
            file_->seek(delta, fileOrigin);
            return aiReturn_SUCCESS;
        }
        catch (const std::exception& error)
        {
            state_.fail(path_, error.what());
            return aiReturn_FAILURE;
        }
    }

    size_t AssimpFileStream::Tell() const
    {
        try
        {
            return static_cast<size_t>(file_->tell());
        }
        catch (const std::exception& error)
        {
            state_.fail(path_, error.what());
            return 0;
        }
    }

    size_t AssimpFileStream::FileSize() const
    {
        return size_;
    }

    void AssimpFileStream::Flush() {}

    AssimpFileIO::AssimpFileIO(FileIO::FileIO& fileIO, std::string modelPath, bool gltf)
        : fileIO_(fileIO), modelPath_(std::move(modelPath)), gltf_(gltf)
    {}

    std::string AssimpFileIO::physicalPath(std::string_view path) const
    {
        std::string decoded(path);
        if (gltf_ && path != modelPath_)
        {
            // Assimp prepends the already decoded model directory to the URI.
            const auto prefix = Path::getParentPath(modelPath_) + '/';
            decoded =
                path.starts_with(prefix) ? prefix + decodeModelUri(path.substr(prefix.size())) : decodeModelUri(path);
        }
        if (decoded.find(':') != std::string::npos || decoded.find('\0') != std::string::npos ||
            Path::isAbsolute(decoded))
        {
            throw Error::Exception(Error::Code::IO, "Absolute or network model dependency is unsupported");
        }
        auto normalized = Path::normalize(decoded);
        if (!normalized.starts_with("assets/"))
        {
            throw Error::Exception(Error::Code::IO, "Model dependency escapes assets root");
        }
        return normalized;
    }

    bool AssimpFileIO::Exists(const char* path) const
    {
        try
        {
            return fileIO_.exists(Utils::Utf8::toFsPath(physicalPath(path)));
        }
        catch (const std::exception& error)
        {
            state_.fail(path, error.what());
            return false;
        }
    }

    char AssimpFileIO::getOsSeparator() const
    {
        return '/';
    }

    Assimp::IOStream* AssimpFileIO::Open(const char* path, const char* mode)
    {
        try
        {
            if (std::string_view(mode) != "r" && std::string_view(mode) != "rb" && std::string_view(mode) != "rt")
            {
                throw Error::Exception(Error::Code::IO, "Model input is read-only");
            }
            const auto resolved = physicalPath(path);
            auto file = fileIO_.open(Utils::Utf8::toFsPath(resolved), FileIO::OpenMode::Read, FileIO::ShareMode::Read);
            if (!file)
            {
                throw Error::Exception(Error::Code::IO, "Required model file was not found");
            }
            auto stream = std::make_unique<AssimpFileStream>(std::move(file), resolved, state_);
            if (Path::hasExtension(resolved, "gltf") || Path::hasExtension(resolved, "mtl"))
            {
                std::string text(stream->FileSize(), '\0');
                stream->Read(text.data(), 1, text.size());
                if (!state_.failure.empty())
                {
                    return nullptr;
                }
                checkModelSource(resolved, text, objDiffuseMaterials_);
                if (stream->Seek(0, aiOrigin_SET) != aiReturn_SUCCESS)
                {
                    return nullptr;
                }
            }
            return stream.release();
        }
        catch (const std::exception& error)
        {
            state_.fail(path, error.what());
            return nullptr;
        }
    }

    void AssimpFileIO::Close(Assimp::IOStream* stream)
    {
        delete stream;
    }

    const std::string& AssimpFileIO::failure() const
    {
        return state_.failure;
    }

    bool AssimpFileIO::hasObjDiffuseColor(std::string_view material) const
    {
        return objDiffuseMaterials_.contains(std::string(material));
    }
}  // namespace Incubator::Resource::Detail
