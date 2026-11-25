#pragma once

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
#include "runtime/resource/asset_importer.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/resource/loader/model_resource.h"
#include "runtime/resource/loader/texture_loader.h"
#include "runtime/resource/loader/texture_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"


namespace Incubator::Test
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
        std::string reparseDirectory;
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
            const auto utf8 = Utils::Utf8::toUtf8String(path.lexically_normal().generic_wstring());
            counters.paths.push_back(utf8);
            auto file = inner_->open(path, mode, share);
            if (!file)
                return nullptr;
            return std::make_unique<TrackedHandle>(
                std::move(file), counters,
                !counters.failPath.empty() && utf8.find(counters.failPath) != std::string::npos);
        }
        std::vector<Fio::DirectoryEntry> listDirectory(const std::filesystem::path& path) override
        {
            return inner_->listDirectory(path);
        }
        std::optional<Fio::DirectoryEntry> inspect(const fs::path& path) override
        {
            auto entry = inner_->inspect(path);
            if (entry && entry->directory && entry->name == reparseDirectory)
                entry->reparsePoint = true;
            return entry;
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
            source_ = std::make_unique<AssetSource>(fileIO_);
            ResourceManager::instance().registerLoader(ResourceType::MODEL, std::make_unique<ModelLoader>(*source_));
        }
        void copyDemoModel(std::string_view directory)
        {
            const auto source = Utils::Utf8::toFsPath(MODEL_DEMO_ASSET_DIR) / "models" / std::string(directory);
            const auto destination = temporary_ / "assets/models" / std::string(directory);
            fs::copy(source, destination, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        }
        void TearDown() override
        {
            holds_.clear();
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
        void editGltf(const std::function<void(rapidjson::Document&)>& edit, std::string_view path = GLTF)
        {
            rapidjson::Document source;
            const auto text = read(path);
            source.Parse(text.c_str());
            ASSERT_FALSE(source.HasParseError());
            edit(source);
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            source.Accept(writer);
            write(path, buffer.GetString());
        }
        const ModelResource* load(std::string_view path)
        {
            holds_.push_back(ResourceManager::instance().hold<ModelResource>(ResourceType::MODEL, path));
            id_ = ResourceRegistry::instance().lookup(ResourceType::MODEL, path);
            return holds_.back().get<ModelResource>();
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
            EXPECT_EQ(ResourceRegistry::instance().refCount(id_), 0u);
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
        std::unique_ptr<AssetSource> source_;
        ResourceId id_;
        std::vector<CpuResource> holds_;
        std::ostringstream messages_;
    };

    class AssetImporterTest : public ModelLoaderTest
    {
    protected:
        void writeExternal(std::string_view name, std::string_view bytes)
        {
            write(std::string("../external/") + std::string(name), bytes);
        }

        fs::path external(std::string_view name)
        {
            return temporary_ / "external" / Utils::Utf8::toFsPath(name);
        }

        void expectImportFailure(std::string_view name, std::string_view reason)
        {
            try
            {
                importExternalModel(*source_, external(name), {"new/imports"});
                ADD_FAILURE() << "Expected import to fail for " << name;
            }
            catch (const Error::Exception& error)
            {
                EXPECT_EQ(error.code(), Error::Code::IO);
                EXPECT_NE(error.message().find(reason), std::string::npos) << error.what();
            }
            EXPECT_FALSE(fs::exists(source_->root() / "new"));
            expectClosed();
        }

        void expectModelTexture(const ImportedModelResult& imported, std::string_view texturePath)
        {
            const auto* model = load(imported.virtualPath);
            ASSERT_NE(model, nullptr) << messages_.str();
            ASSERT_EQ(model->parts().size(), 2u);
            EXPECT_EQ(model->materials()[model->parts()[0].material].texturePath, texturePath);
            expectClosed();
        }
    };
}  // namespace Incubator::Test
