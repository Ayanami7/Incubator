#include "runtime/resource/loader/detail/assimp_fileio.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include "runtime/core/error/error.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource::Detail;
    namespace Fio = Incubator::FileIO;
    using ::testing::_;
    using ::testing::Return;

    class MemoryHandle final : public Fio::FileHandle
    {
    public:
        explicit MemoryHandle(size_t& destroyed) : destroyed_(destroyed) {}
        ~MemoryHandle() override
        {
            ++destroyed_;
        }
        size_t position = 0;
        size_t chunk = 3;
        bool earlyEof = false;
        bool throwRead = false;
        bool throwSeek = false;
        bool isValid() const override
        {
            return true;
        }
        size_t read(std::span<uint8_t> output) override
        {
            if (throwRead)
                throw Error::Exception(Error::Code::IO, "controlled IO exception");
            if (earlyEof)
                return 0;
            const size_t count = std::min({output.size(), chunk, data_.size() - position});
            std::memcpy(output.data(), data_.data() + position, count);
            position += count;
            return count;
        }
        size_t write(std::span<const uint8_t>) override
        {
            return 0;
        }
        size_t seek(int64_t offset, Fio::SeekOrigin origin) override
        {
            if (throwSeek)
                throw Error::Exception(Error::Code::IO, "controlled seek exception");
            const int64_t base = origin == Fio::SeekOrigin::Begin     ? 0
                                 : origin == Fio::SeekOrigin::Current ? position
                                                                      : data_.size();
            position = static_cast<size_t>(base + offset);
            return position;
        }
        uint64_t tell() const override
        {
            return position;
        }
        uint64_t size() const override
        {
            return data_.size();
        }
        void close() override {}
        void flush() override {}

    private:
        std::string data_ = "0123456789";
        size_t& destroyed_;
    };

    class MockFileIO : public Fio::FileIO
    {
    public:
        MOCK_METHOD(bool, exists, (const std::filesystem::path&), (override));
        MOCK_METHOD(uint64_t, getFileSize, (const std::filesystem::path&), (override));
        MOCK_METHOD(bool, remove, (const std::filesystem::path&), (override));
        MOCK_METHOD(std::unique_ptr<Fio::FileHandle>, open,
                    (const std::filesystem::path&, Fio::OpenMode, Fio::ShareMode), (override));
        MOCK_METHOD(std::vector<Fio::DirectoryEntry>, listDirectory, (const std::filesystem::path&), (override));
        MOCK_METHOD(bool, directoryExists, (const std::filesystem::path&), (override));
        MOCK_METHOD(bool, createDirectory, (const std::filesystem::path&), (override));
        MOCK_METHOD(bool, removeDirectory, (const std::filesystem::path&), (override));
    };

    TEST(AssimpFileIOTest, ReadAccumulatesChunksAndReturnsCompleteElementCount)
    {
        size_t destroyed = 0;
        ImportIoState state;
        {
            AssimpFileStream stream(std::make_unique<MemoryHandle>(destroyed), "assets/mesh.bin", state);
            EXPECT_EQ(stream.FileSize(), 10u);
            EXPECT_EQ(stream.Tell(), 0u);
            std::array<char, 12> data{};
            EXPECT_EQ(stream.Read(data.data(), 4, 3), 2u);
            EXPECT_EQ(std::string(data.data(), 10), "0123456789");
            EXPECT_EQ(stream.Tell(), 10u);
            EXPECT_EQ(stream.Read(data.data(), 1, 1), 0u);
            EXPECT_EQ(stream.Read(data.data(), 0, 1), 0u);
            EXPECT_TRUE(state.failure.empty());
        }
        EXPECT_EQ(destroyed, 1u);
    }
    TEST(AssimpFileIOTest, SeekMapsBeginCurrentAndSignedEndOffsets)
    {
        size_t destroyed = 0;
        ImportIoState state;
        AssimpFileStream stream(std::make_unique<MemoryHandle>(destroyed), "assets/mesh.bin", state);
        EXPECT_EQ(stream.Seek(4, aiOrigin_SET), aiReturn_SUCCESS);
        EXPECT_EQ(stream.Tell(), 4u);
        EXPECT_EQ(stream.Seek(2, aiOrigin_CUR), aiReturn_SUCCESS);
        EXPECT_EQ(stream.Tell(), 6u);
        EXPECT_EQ(stream.Seek(static_cast<size_t>(-2), aiOrigin_END), aiReturn_SUCCESS);
        EXPECT_EQ(stream.Tell(), 8u);
        EXPECT_EQ(stream.Seek(static_cast<size_t>(-3), aiOrigin_CUR), aiReturn_SUCCESS);
        EXPECT_EQ(stream.Tell(), 5u);
        EXPECT_EQ(stream.Seek(0, aiOrigin_END), aiReturn_SUCCESS);
        EXPECT_EQ(stream.Tell(), 10u);
        EXPECT_TRUE(state.failure.empty());
    }
    TEST(AssimpFileIOTest, SeekFailurePreservesPositionAndContext)
    {
        size_t destroyed = 0;
        ImportIoState state;
        auto handle = std::make_unique<MemoryHandle>(destroyed);
        auto* borrowed = handle.get();
        AssimpFileStream stream(std::move(handle), "assets/mesh.bin", state);
        EXPECT_EQ(stream.Seek(11, aiOrigin_SET), aiReturn_FAILURE);
        EXPECT_EQ(stream.Tell(), 0u);
        EXPECT_NE(state.failure.find("outside file bounds"), std::string::npos);
        state.failure.clear();
        borrowed->throwSeek = true;
        EXPECT_EQ(stream.Seek(0, aiOrigin_SET), aiReturn_FAILURE);
        EXPECT_NE(state.failure.find("controlled seek exception"), std::string::npos);
    }
    TEST(AssimpFileIOTest, UnexpectedEofAndReadExceptionAreRetainedForImporter)
    {
        size_t destroyed = 0;
        ImportIoState state;
        auto handle = std::make_unique<MemoryHandle>(destroyed);
        auto* borrowed = handle.get();
        AssimpFileStream stream(std::move(handle), "assets/mesh.bin", state);
        borrowed->earlyEof = true;
        std::array<char, 10> bytes{};
        EXPECT_EQ(stream.Read(bytes.data(), 1, 10), 0u);
        EXPECT_NE(state.failure.find("Incomplete model read"), std::string::npos);
        state.failure.clear();
        borrowed->earlyEof = false;
        borrowed->throwRead = true;
        EXPECT_EQ(stream.Read(bytes.data(), 1, 10), 0u);
        EXPECT_NE(state.failure.find("assets/mesh.bin"), std::string::npos);
        EXPECT_NE(state.failure.find("controlled IO exception"), std::string::npos);
    }
    TEST(AssimpFileIOTest, RejectsWriteAndReadMultiplicationOverflow)
    {
        size_t destroyed = 0;
        ImportIoState state;
        AssimpFileStream stream(std::make_unique<MemoryHandle>(destroyed), "assets/mesh.bin", state);
        char byte{};
        EXPECT_EQ(stream.Read(&byte, std::numeric_limits<size_t>::max(), 2), 0u);
        EXPECT_NE(state.failure.find("overflow"), std::string::npos);
        state.failure.clear();
        EXPECT_EQ(stream.Write(&byte, 1, 1), 0u);
        EXPECT_NE(state.failure.find("read-only"), std::string::npos);
    }
    TEST(AssimpFileIOTest, MissingExistenceProbeDoesNotBecomeImportFailure)
    {
        ::testing::StrictMock<MockFileIO> fileIO;
        Resource::AssetSource source(fileIO);
        AssimpFileIO io(source, "models/model.obj", false);
        EXPECT_CALL(fileIO, exists(_)).WillOnce(Return(false));
        EXPECT_FALSE(io.Exists("models/optional.mtl"));
        EXPECT_TRUE(io.failure().empty());
    }
    TEST(AssimpFileIOTest, RequiredOpenFailureIsRecordedAndCloseDestroysHandle)
    {
        ::testing::StrictMock<MockFileIO> fileIO;
        Resource::AssetSource source(fileIO);
        AssimpFileIO io(source, "models/model.obj", false);
        EXPECT_CALL(fileIO, open(_, Fio::OpenMode::Read, Fio::ShareMode::Read)).WillOnce(Return(nullptr));
        EXPECT_EQ(io.Open("models/missing.mtl"), nullptr);
        EXPECT_NE(io.failure().find("Required model file"), std::string::npos);
        size_t destroyed = 0;
        EXPECT_CALL(fileIO, open(_, Fio::OpenMode::Read, Fio::ShareMode::Read)).WillOnce([&](const auto&, auto, auto) {
            return std::make_unique<MemoryHandle>(destroyed);
        });
        auto* stream = io.Open("models/mesh.bin");
        ASSERT_NE(stream, nullptr);
        io.Close(stream);
        EXPECT_EQ(destroyed, 1u);
    }
    TEST(AssimpFileIOTest, RejectsEscapingOrNetworkPathsBeforeNativeOpen)
    {
        ::testing::StrictMock<MockFileIO> fileIO;
        Resource::AssetSource source(fileIO);
        for (const char* path : {"models/../../../outside.bin", "models/C:/outside.bin",
                                 "models/https://example.invalid/bin", "//server/file.bin"})
        {
            AssimpFileIO io(source, "models/model.obj", false);
            EXPECT_EQ(io.Open(path), nullptr);
            EXPECT_FALSE(io.failure().empty());
        }
    }
    TEST(AssetDirectoryTest, FiltersReparseDirectoriesAndRejectsNestedEntryBeforeFollowingLink)
    {
        ::testing::StrictMock<MockFileIO> fileIO;
        Resource::AssetSource source(fileIO);
        EXPECT_CALL(fileIO, listDirectory(source.root()))
            .WillRepeatedly(
                Return(std::vector<Fio::DirectoryEntry>{{"linked", true, true}, {"ordinary", true, false}}));
        const auto items = source.listDirectory();
        ASSERT_EQ(items.size(), 1u);
        EXPECT_EQ(items[0].name, "ordinary");
        EXPECT_THROW(source.listDirectory("linked/child"), Error::Exception);
    }
}  // namespace
