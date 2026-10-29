#include "runtime/resource/loader/texture_loader.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/resource/loader/texture_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"

namespace
{
    namespace fs = std::filesystem;
    using namespace Incubator;
    using namespace Incubator::Resource;
    using ::testing::_;
    using ::testing::ByMove;
    using ::testing::Return;

    const std::vector<uint8_t> REFERENCE_PIXELS = {
        255, 0,   0,   255, 0,   255, 0,   255, 0,   0,   255, 255, 255, 255, 0,   255,
        0,   255, 255, 255, 255, 0,   255, 255, 128, 128, 128, 255, 255, 255, 255, 255,
        255, 128, 0,   255, 128, 0,   255, 128, 17,  34,  51,  0,   0,   0,   0,   255,
    };

    std::vector<uint8_t> readBytes(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error("Cannot open texture fixture: " + Utils::Utf8::toUtf8String(path));
        }
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    uint32_t chunkCrc(std::span<const uint8_t> bytes)
    {
        uint32_t crc = 0xffffffffu;
        for (uint8_t byte : bytes)
        {
            crc ^= byte;
            for (int bit = 0; bit < 8; ++bit)
            {
                crc = (crc >> 1u) ^ ((crc & 1u) ? 0xedb88320u : 0u);
            }
        }
        return ~crc;
    }

    void writeBigEndian(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
    {
        for (size_t i = 0; i < 4; ++i)
        {
            bytes[offset + i] = static_cast<uint8_t>(value >> (24u - i * 8u));
        }
    }

    std::vector<uint8_t> pngChunk(std::string_view type, std::span<const uint8_t> payload)
    {
        std::vector<uint8_t> chunk(4);
        writeBigEndian(chunk, 0, static_cast<uint32_t>(payload.size()));
        chunk.insert(chunk.end(), type.begin(), type.end());
        chunk.insert(chunk.end(), payload.begin(), payload.end());
        const uint32_t crc = chunkCrc(std::span<const uint8_t>(chunk).subspan(4));
        const size_t crcOffset = chunk.size();
        chunk.resize(crcOffset + 4);
        writeBigEndian(chunk, crcOffset, crc);
        return chunk;
    }

    std::vector<uint8_t> storedFixture(std::string_view name)
    {
        return readBytes(Utils::Utf8::toFsPath(TEXTURE_TEST_FIXTURE_DIR) /
                         Utils::Utf8::toFsPath(std::string(name) + ".png"));
    }

    // 在内存中派生字节级变体；磁盘上只保存编码格式不同的文件。
    std::vector<uint8_t> fixture(std::string_view name)
    {
        if (name == "rgb_trns" || name == "gray_trns")
        {
            auto bytes = storedFixture(name == "rgb_trns" ? "rgb8" : "gray8");
            const std::vector<uint8_t> key =
                name == "rgb_trns" ? std::vector<uint8_t>{0, 255, 0, 0, 0, 0} : std::vector<uint8_t>{0, 128};
            const auto chunk = pngChunk("tRNS", key);
            bytes.insert(bytes.begin() + 33, chunk.begin(), chunk.end());
            return bytes;
        }
        if (name == "gray2" || name == "gray4")
        {
            auto bytes = storedFixture("gray1");
            bytes[24] = name == "gray2" ? 2 : 4;
            writeBigEndian(bytes, 29, chunkCrc(std::span<const uint8_t>(bytes).subspan(12, 17)));
            return bytes;
        }
        const bool derived = name == "rgba8" || name == "color_metadata" || name == "truncated_signature" ||
                             name == "truncated_ihdr" || name == "truncated_idat" || name == "invalid_signature" ||
                             name == "invalid_zlib_header" || name == "crc_only" || name == "missing_iend" ||
                             name == "missing_iend_crc" || name == "trailing_bytes" || name == "zero_width" ||
                             name == "zero_height" || name == "oversize_width" || name == "oversize_height" ||
                             name == "huge_dimensions" || name == "invalid_compression" || name == "invalid_filter" ||
                             name == "invalid_ihdr_length" || name == "not_ihdr_first";
        if (!derived)
        {
            return storedFixture(name);
        }
        auto bytes = readBytes(Utils::Utf8::toFsPath(TEXTURE_REFERENCE_FILE));
        if (name == "truncated_signature" || name == "truncated_ihdr" || name == "truncated_idat")
        {
            bytes.resize(name == "truncated_signature" ? 7 : name == "truncated_ihdr" ? 20 : 72);
        }
        else if (name == "invalid_signature")
        {
            bytes[0] = 'X';
        }
        else if (name == "invalid_zlib_header")
        {
            bytes[41] = bytes[42] = 0;
            writeBigEndian(bytes, 103, chunkCrc(std::span<const uint8_t>(bytes).subspan(37, 66)));
        }
        else if (name == "crc_only")
        {
            bytes[103] ^= 1;
        }
        else if (name == "missing_iend" || name == "missing_iend_crc")
        {
            bytes.resize(bytes.size() - (name == "missing_iend" ? 12 : 4));
        }
        else if (name == "trailing_bytes")
        {
            constexpr std::string_view TAIL = "not-a-png-chunk";
            bytes.insert(bytes.end(), TAIL.begin(), TAIL.end());
        }
        else if (name == "not_ihdr_first")
        {
            const std::vector<uint8_t> text = {'f', 'i', 'x', 't', 'u', 'r', 'e', 0, 't', 'e', 'x', 't'};
            const auto chunk = pngChunk("tEXt", text);
            bytes.insert(bytes.begin() + 8, chunk.begin(), chunk.end());
        }
        else if (name == "color_metadata")
        {
            std::vector<uint8_t> gamma(4);
            writeBigEndian(gamma, 0, 100000);
            auto chunks = pngChunk("gAMA", gamma);
            std::vector<uint8_t> chromaticity(32);
            constexpr uint32_t VALUES[] = {31270, 32900, 64000, 33000, 30000, 60000, 15000, 6000};
            for (size_t i = 0; i < 8; ++i)
            {
                writeBigEndian(chromaticity, i * 4, VALUES[i]);
            }
            const auto chrm = pngChunk("cHRM", chromaticity);
            const auto srgb = pngChunk("sRGB", std::vector<uint8_t>{0});
            chunks.insert(chunks.end(), chrm.begin(), chrm.end());
            chunks.insert(chunks.end(), srgb.begin(), srgb.end());
            bytes.insert(bytes.begin() + 33, chunks.begin(), chunks.end());
        }
        else if (name != "rgba8")
        {
            if (name == "zero_width" || name == "oversize_width" || name == "huge_dimensions")
            {
                writeBigEndian(bytes, 16, name == "zero_width" ? 0 : name == "oversize_width" ? 4097 : 0xffffffffu);
            }
            if (name == "zero_height" || name == "oversize_height" || name == "huge_dimensions")
            {
                writeBigEndian(bytes, 20, name == "zero_height" ? 0 : name == "oversize_height" ? 4097 : 0xffffffffu);
            }
            if (name == "invalid_compression" || name == "invalid_filter")
            {
                bytes[name == "invalid_compression" ? 26 : 27] = 1;
            }
            writeBigEndian(bytes, 29, chunkCrc(std::span<const uint8_t>(bytes).subspan(12, 17)));
            if (name == "invalid_ihdr_length")
            {
                writeBigEndian(bytes, 8, 12);
            }
        }
        return bytes;
    }

    void expectSingleFailure(const std::string& messages, std::string_view reason, std::string_view path)
    {
        const auto first = messages.find("load: loader threw:");
        ASSERT_NE(first, std::string::npos);
        EXPECT_EQ(messages.find("load: loader threw:", first + 1), std::string::npos);
        EXPECT_NE(messages.find(reason), std::string::npos);
        EXPECT_NE(messages.find(path), std::string::npos);
    }

    class TextureLoaderTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            Log::Registry::reset();
            Log::Registry::get("resource").addSink(std::make_shared<spdlog::sinks::ostream_sink_mt>(messages_));
            previousDirectory_ = fs::current_path();
            testDirectory_ =
                fs::temp_directory_path() /
                ("incubator-texture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            fs::create_directories(testDirectory_ / "assets");
            fs::current_path(testDirectory_);
            fileIO_ = FileIO::createPlatformFileIO();
            source_ = std::make_unique<AssetSource>(*fileIO_);
            ResourceManager::instance().registerLoader(ResourceType::TEXTURE,
                                                       std::make_unique<TextureLoader>(*source_));
            id_ = ResourceManager::instance().registerResource(ResourceType::TEXTURE, "纹理/测试.png");
            ASSERT_TRUE(id_.valid());
        }

        void TearDown() override
        {
            ResourceManager::instance().release(id_);
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            Log::Registry::reset();
            source_.reset();
            fileIO_.reset();
            fs::current_path(previousDirectory_);
            fs::remove_all(testDirectory_);
        }

        void writeBytes(const std::vector<uint8_t>& bytes)
        {
            const auto path = Utils::Utf8::toFsPath("assets/纹理/测试.png");
            fs::create_directories(path.parent_path());
            std::ofstream output(path, std::ios::binary);
            output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            ASSERT_TRUE(output.good());
        }

        TextureResource* loadFixture(std::string_view name)
        {
            writeBytes(fixture(name));
            return ResourceManager::instance().get<TextureResource>(id_);
        }

        fs::path previousDirectory_;
        fs::path testDirectory_;
        std::unique_ptr<FileIO::FileIO> fileIO_;
        std::unique_ptr<AssetSource> source_;
        ResourceId id_;
        std::ostringstream messages_;
    };

    TEST_F(TextureLoaderTest, ChinesePathLoadsReferenceWithExactRgbaAndTopToBottomRows)
    {
        const auto* texture = loadFixture("rgba8");
        ASSERT_NE(texture, nullptr);
        EXPECT_EQ(texture->id(), id_);
        EXPECT_EQ(texture->type(), ResourceType::TEXTURE);
        EXPECT_EQ(texture->state(), ResourceState::READY);
        EXPECT_EQ(texture->width(), 4u);
        EXPECT_EQ(texture->height(), 3u);
        EXPECT_EQ(texture->pixels(), REFERENCE_PIXELS);
        EXPECT_TRUE(messages_.str().empty());
    }

    TEST_F(TextureLoaderTest, CacheReferenceCountingAndAcquireAfterUnloadFollowManagerContract)
    {
        auto& manager = ResourceManager::instance();
        const auto* first = loadFixture("rgba8");
        ASSERT_NE(first, nullptr);
        EXPECT_EQ(manager.registerResource(ResourceType::TEXTURE, "纹理/测试.png"), id_);
        EXPECT_EQ(ResourceRegistry::instance().refCount(id_), 1u);
        // 修改文件不会使已挂载对象失效。
        writeBytes(fixture("rgb8"));
        EXPECT_EQ(manager.get<TextureResource>(id_), first);
        EXPECT_EQ(first->pixels(), REFERENCE_PIXELS);
        manager.release(id_);
        EXPECT_EQ(ResourceRegistry::instance().getObject(id_), nullptr);
        EXPECT_EQ(ResourceRegistry::instance().refCount(id_), 0u);
        ResourceType type = ResourceType::INVALID;
        std::string path;
        ASSERT_TRUE(ResourceRegistry::instance().lookup(id_, type, path));
        EXPECT_EQ(type, ResourceType::TEXTURE);
        EXPECT_EQ(path, "纹理/测试.png");
        EXPECT_EQ(manager.registerResource(ResourceType::TEXTURE, path), id_);
        EXPECT_EQ(ResourceRegistry::instance().refCount(id_), 0u);
        manager.acquire(id_);
        const auto* rebuilt = manager.get<TextureResource>(id_);
        ASSERT_NE(rebuilt, nullptr);
        EXPECT_EQ(rebuilt->width(), 2u);
        EXPECT_EQ(rebuilt->pixels(), (std::vector<uint8_t>{255, 0, 0, 255, 128, 128, 128, 255}));
    }

    TEST_F(TextureLoaderTest, DirectFailureThrowsWithoutLoggingAndManagerLogsMissingPathOnce)
    {
        TextureLoader direct(*source_);
        EXPECT_THROW(direct.load(id_, "纹理/测试.png"), Error::Exception);
        EXPECT_TRUE(messages_.str().empty());
        EXPECT_EQ(ResourceManager::instance().load(id_), nullptr);
        EXPECT_EQ(ResourceRegistry::instance().getObject(id_), nullptr);
        expectSingleFailure(messages_.str(), "PNG file was not found", "assets/纹理/测试.png");
    }

    TEST_F(TextureLoaderTest, FailedDecodeDoesNotMountAndCorrectedFileCanBeRetried)
    {
        EXPECT_EQ(loadFixture("invalid_zlib_header"), nullptr);
        EXPECT_EQ(ResourceRegistry::instance().getObject(id_), nullptr);
        expectSingleFailure(messages_.str(), "bad compression", "assets/纹理/测试.png");
        const auto* texture = loadFixture("rgba8");
        ASSERT_NE(texture, nullptr);
        EXPECT_EQ(texture->pixels(), REFERENCE_PIXELS);
    }

    using ChannelCase = std::tuple<std::string, std::vector<uint8_t>>;
    class TextureChannelTest : public TextureLoaderTest, public ::testing::WithParamInterface<ChannelCase>
    {};

    TEST_P(TextureChannelTest, ConvertsSupportedChannelsToStraightRgba)
    {
        const auto* texture = loadFixture(std::get<0>(GetParam()));
        ASSERT_NE(texture, nullptr);
        EXPECT_EQ(texture->width(), 2u);
        EXPECT_EQ(texture->height(), 1u);
        EXPECT_EQ(texture->pixels(), std::get<1>(GetParam()));
    }

    INSTANTIATE_TEST_SUITE_P(SupportedPng, TextureChannelTest,
                             ::testing::Values(ChannelCase{"gray8", {0, 0, 0, 255, 128, 128, 128, 255}},
                                               ChannelCase{"gray_alpha8", {128, 128, 128, 64, 255, 255, 255, 0}},
                                               ChannelCase{"rgb8", {255, 0, 0, 255, 128, 128, 128, 255}},
                                               ChannelCase{"rgb_trns", {255, 0, 0, 0, 128, 128, 128, 255}},
                                               ChannelCase{"gray_trns", {0, 0, 0, 255, 128, 128, 128, 0}}),
                             [](const auto& info) { return std::get<0>(info.param); });

    class TextureCompatibleInputTest : public TextureLoaderTest, public ::testing::WithParamInterface<std::string>
    {};

    TEST_P(TextureCompatibleInputTest, MetadataAndToleratedIntegrityChangesPreserveExactPixels)
    {
        const auto* texture = loadFixture(GetParam());
        ASSERT_NE(texture, nullptr);
        EXPECT_EQ(texture->pixels(), REFERENCE_PIXELS);
    }

    INSTANTIATE_TEST_SUITE_P(CompatiblePng, TextureCompatibleInputTest,
                             ::testing::Values("color_metadata", "icc_metadata", "crc_only", "missing_iend_crc",
                                               "trailing_bytes"),
                             [](const auto& info) { return info.param; });

    using FailureCase = std::tuple<std::string, std::string>;
    class TextureFailureTest : public TextureLoaderTest, public ::testing::WithParamInterface<FailureCase>
    {};

    TEST_P(TextureFailureTest, RejectsBeforeMountingAndPreservesChinesePathAndReason)
    {
        EXPECT_EQ(loadFixture(std::get<0>(GetParam())), nullptr);
        EXPECT_EQ(ResourceRegistry::instance().getObject(id_), nullptr);
        expectSingleFailure(messages_.str(), std::get<1>(GetParam()), "assets/纹理/测试.png");
    }

    INSTANTIATE_TEST_SUITE_P(
        InvalidPng, TextureFailureTest,
        ::testing::Values(FailureCase{"truncated_signature", "incomplete PNG signature"},
                          FailureCase{"truncated_ihdr", "Incomplete PNG IHDR"},
                          FailureCase{"truncated_idat", "PNG decode failed (outofdata)"},
                          FailureCase{"invalid_signature", "Not a PNG"},
                          FailureCase{"invalid_zlib_header", "PNG decode failed (bad compression)"},
                          FailureCase{"missing_iend", "PNG decode failed (decoder supplied no reason)"},
                          FailureCase{"palette8", "Unsupported PNG input"},
                          FailureCase{"gray1", "Unsupported PNG input"}, FailureCase{"gray2", "Unsupported PNG input"},
                          FailureCase{"gray4", "Unsupported PNG input"}, FailureCase{"rgb16", "Unsupported PNG input"},
                          FailureCase{"rgba16", "Unsupported PNG input"}, FailureCase{"adam7", "Unsupported PNG input"},
                          FailureCase{"cgbi", "Invalid PNG IHDR"},
                          FailureCase{"zero_width", "Invalid PNG dimensions: 0x3"},
                          FailureCase{"zero_height", "Invalid PNG dimensions: 4x0"},
                          FailureCase{"oversize_width", "Invalid PNG dimensions: 4097x3"},
                          FailureCase{"oversize_height", "Invalid PNG dimensions: 4x4097"},
                          FailureCase{"huge_dimensions", "Invalid PNG dimensions"},
                          FailureCase{"invalid_compression", "Unsupported PNG input"},
                          FailureCase{"invalid_filter", "Unsupported PNG input"},
                          FailureCase{"invalid_ihdr_length", "Invalid PNG IHDR"},
                          FailureCase{"not_ihdr_first", "Invalid PNG IHDR"}),
        [](const auto& info) { return std::get<0>(info.param); });

    TEST_F(TextureLoaderTest, AcceptsMaximumWidthOrHeightWithSmallPayloads)
    {
        auto& manager = ResourceManager::instance();
        const auto* wide = loadFixture("max_width");
        ASSERT_NE(wide, nullptr);
        EXPECT_EQ(wide->width(), 4096u);
        EXPECT_EQ(wide->height(), 1u);
        EXPECT_EQ(wide->pixels().size(), 16384u);
        EXPECT_EQ(wide->pixels()[0], 128);
        EXPECT_EQ(wide->pixels()[3], 255);
        manager.release(id_);
        manager.acquire(id_);
        const auto* tall = loadFixture("max_height");
        ASSERT_NE(tall, nullptr);
        EXPECT_EQ(tall->width(), 1u);
        EXPECT_EQ(tall->height(), 4096u);
        EXPECT_EQ(tall->pixels().size(), 16384u);
        EXPECT_EQ(tall->pixels().back(), 255);
    }

    TEST_F(TextureLoaderTest, BuildDeploysReferenceAndSpirvUnderSameReadableAssetRoot)
    {
        const auto root = Utils::Utf8::toFsPath(TEXTURE_BUILD_ASSET_DIR);
        const auto bytes = readBytes(root / "textures/reference_rgba.png");
        EXPECT_EQ(bytes, fixture("rgba8"));
        // 将变异数据的 CRC 辅助函数结果与独立生成的 IHDR CRC 对比。
        std::vector<uint8_t> expectedCrc(4);
        writeBigEndian(expectedCrc, 0, chunkCrc(std::span<const uint8_t>(bytes).subspan(12, 17)));
        EXPECT_TRUE(std::equal(expectedCrc.begin(), expectedCrc.end(), bytes.begin() + 29));
        auto texture = fileIO_->open(root / "textures/reference_rgba.png", FileIO::OpenMode::Read);
        ASSERT_NE(texture, nullptr);
        EXPECT_EQ(texture->size(), 119u);
        auto vertex = fileIO_->open(root / "shaders/cube.vert.spv", FileIO::OpenMode::Read);
        auto fragment = fileIO_->open(root / "shaders/cube.frag.spv", FileIO::OpenMode::Read);
        ASSERT_NE(vertex, nullptr);
        ASSERT_NE(fragment, nullptr);
        EXPECT_GT(vertex->size(), 0u);
        EXPECT_GT(fragment->size(), 0u);
    }

    class MockFileHandle : public FileIO::FileHandle
    {
    public:
        MOCK_METHOD(bool, isValid, (), (const, override));
        MOCK_METHOD(size_t, read, (std::span<uint8_t>), (override));
        MOCK_METHOD(size_t, write, (std::span<const uint8_t>), (override));
        MOCK_METHOD(size_t, seek, (int64_t, FileIO::SeekOrigin), (override));
        MOCK_METHOD(uint64_t, tell, (), (const, override));
        MOCK_METHOD(uint64_t, size, (), (const, override));
        MOCK_METHOD(void, close, (), (override));
        MOCK_METHOD(void, flush, (), (override));
    };

    class MockFileIO : public FileIO::FileIO
    {
    public:
        MOCK_METHOD(bool, exists, (const fs::path&), (override));
        MOCK_METHOD(uint64_t, getFileSize, (const fs::path&), (override));
        MOCK_METHOD(bool, remove, (const fs::path&), (override));
        MOCK_METHOD(std::unique_ptr<Incubator::FileIO::FileHandle>, open,
                    (const fs::path&, Incubator::FileIO::OpenMode, Incubator::FileIO::ShareMode), (override));
        MOCK_METHOD(bool, directoryExists, (const fs::path&), (override));
        MOCK_METHOD(bool, createDirectory, (const fs::path&), (override));
        MOCK_METHOD(bool, removeDirectory, (const fs::path&), (override));
    };

    class TextureReadTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            Log::Registry::reset();
            Log::Registry::get("resource").addSink(std::make_shared<spdlog::sinks::ostream_sink_mt>(messages_));
            source_ = std::make_unique<AssetSource>(fileIO_);
            ResourceManager::instance().registerLoader(ResourceType::TEXTURE,
                                                       std::make_unique<TextureLoader>(*source_));
            id_ = ResourceManager::instance().registerResource(ResourceType::TEXTURE, "纹理/短读.png");
        }

        void TearDown() override
        {
            ResourceManager::instance().release(id_);
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            Log::Registry::reset();
        }

        MockFileHandle* prepareHandle(uint64_t size)
        {
            auto handle = std::make_unique<::testing::StrictMock<MockFileHandle>>();
            auto* raw = handle.get();
            EXPECT_CALL(*raw, size()).WillOnce(Return(size));
            EXPECT_CALL(fileIO_,
                        open(source_->resolve("纹理/短读.png"), FileIO::OpenMode::Read, FileIO::ShareMode::Read))
                .WillOnce(Return(ByMove(std::move(handle))));
            return raw;
        }

        void expectFailure(std::string_view reason)
        {
            EXPECT_EQ(ResourceManager::instance().load(id_), nullptr);
            EXPECT_EQ(ResourceRegistry::instance().getObject(id_), nullptr);
            expectSingleFailure(messages_.str(), reason, "assets/纹理/短读.png");
        }

        ::testing::StrictMock<MockFileIO> fileIO_;
        std::unique_ptr<AssetSource> source_;
        ResourceId id_;
        std::ostringstream messages_;
    };

    TEST_F(TextureReadTest, RepeatedShortReadsAccumulateUntilComplete)
    {
        const auto bytes = fixture("rgba8");
        auto* handle = prepareHandle(bytes.size());
        size_t offset = 0;
        EXPECT_CALL(*handle, read(_)).Times(40).WillRepeatedly([&](std::span<uint8_t> buffer) {
            const size_t count = std::min<size_t>(3, buffer.size());
            std::copy_n(bytes.begin() + offset, count, buffer.begin());
            offset += count;
            return count;
        });
        const auto* texture = ResourceManager::instance().get<TextureResource>(id_);
        ASSERT_NE(texture, nullptr);
        EXPECT_EQ(offset, 119u);
        EXPECT_EQ(texture->pixels(), REFERENCE_PIXELS);
    }

    TEST_F(TextureReadTest, EarlyEofReportsExpectedAndActualInsteadOfDecodingPartialData)
    {
        const auto bytes = fixture("rgba8");
        auto* handle = prepareHandle(bytes.size());
        EXPECT_CALL(*handle, read(_))
            .WillOnce([&](std::span<uint8_t> buffer) {
                std::copy_n(bytes.begin(), 72, buffer.begin());
                return 72u;
            })
            .WillOnce(Return(0));
        expectFailure("Incomplete PNG read: expected 119, actual 72");
        EXPECT_EQ(messages_.str().find("PNG decode failed"), std::string::npos);
    }

    class TextureReadExceptionTest : public TextureReadTest, public ::testing::WithParamInterface<bool>
    {};

    TEST_P(TextureReadExceptionTest, ReadExceptionPreservesPathAndCauseWithoutDuplicateLogging)
    {
        auto* handle = prepareHandle(119);
        if (GetParam())
        {
            EXPECT_CALL(*handle, read(_)).WillOnce(Return(7)).WillOnce([](std::span<uint8_t>) -> size_t {
                throw Error::Exception(Error::Code::IO, "injected read failure");
            });
        }
        else
        {
            EXPECT_CALL(*handle, read(_)).WillOnce([](std::span<uint8_t>) -> size_t {
                throw Error::Exception(Error::Code::IO, "injected read failure");
            });
        }
        expectFailure("injected read failure");
    }

    INSTANTIATE_TEST_SUITE_P(ReadBoundaries, TextureReadExceptionTest, ::testing::Bool());

    TEST_F(TextureReadTest, OpenExceptionPreservesChinesePathAndOriginalCause)
    {
        EXPECT_CALL(fileIO_, open(_, _, _))
            .WillOnce([](const fs::path&, FileIO::OpenMode, FileIO::ShareMode) -> std::unique_ptr<FileIO::FileHandle> {
                throw Error::Exception(Error::Code::IO, "injected permission failure");
            });
        expectFailure("injected permission failure");
    }

    TEST_F(TextureReadTest, SizeExceptionPreservesChinesePathAndOriginalCause)
    {
        auto handle = std::make_unique<::testing::StrictMock<MockFileHandle>>();
        EXPECT_CALL(*handle, size()).WillOnce([]() -> uint64_t {
            throw Error::Exception(Error::Code::IO, "injected size failure");
        });
        EXPECT_CALL(fileIO_, open(_, _, _)).WillOnce(Return(ByMove(std::move(handle))));
        expectFailure("injected size failure");
    }

    class TextureInputLimitTest : public TextureReadTest, public ::testing::WithParamInterface<uint64_t>
    {};

    TEST_P(TextureInputLimitTest, RejectsLengthBeforeAllocationOrReading)
    {
        auto* handle = prepareHandle(GetParam());
        EXPECT_CALL(*handle, read(_)).Times(0);
        expectFailure("Invalid PNG input size");
    }

    INSTANTIATE_TEST_SUITE_P(InputBoundaries, TextureInputLimitTest,
                             ::testing::Values(uint64_t{0}, uint64_t{64u * 1024u * 1024u + 1u}));
}  // namespace
