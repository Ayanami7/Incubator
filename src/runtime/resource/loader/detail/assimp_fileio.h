#pragma once

#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <memory>
#include <string>
#include <unordered_set>

#include "runtime/resource/asset_source.h"

namespace Incubator::Resource::Detail
{
    struct ImportIoState
    {
        std::string failure;
        void fail(std::string_view path, std::string_view reason);
    };

    class AssimpFileStream final : public Assimp::IOStream
    {
    public:
        AssimpFileStream(std::unique_ptr<FileIO::FileHandle> file, std::string path, ImportIoState& state);
        size_t Read(void* buffer, size_t size, size_t count) override;
        size_t Write(const void*, size_t, size_t) override;
        aiReturn Seek(size_t offset, aiOrigin origin) override;
        size_t Tell() const override;
        size_t FileSize() const override;
        void Flush() override;

    private:
        std::unique_ptr<FileIO::FileHandle> file_;
        std::string path_;
        ImportIoState& state_;
        size_t size_;
    };

    class AssimpFileIO final : public Assimp::IOSystem
    {
    public:
        AssimpFileIO(AssetSource& source, std::string modelPath, bool gltf);
        bool Exists(const char* path) const override;
        char getOsSeparator() const override;
        Assimp::IOStream* Open(const char* path, const char* mode = "rb") override;
        void Close(Assimp::IOStream* stream) override;
        const std::string& failure() const;
        bool hasObjDiffuseColor(std::string_view material) const;

    private:
        std::string virtualPath(std::string_view path) const;

        AssetSource& source_;
        std::string modelPath_;
        bool gltf_;
        mutable ImportIoState state_;
        std::unordered_set<std::string> objDiffuseMaterials_;
    };
}  // namespace Incubator::Resource::Detail
