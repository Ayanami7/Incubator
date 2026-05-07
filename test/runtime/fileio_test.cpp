#define TESTING_ENABLED

#include "runtime/platform/fileio/fileio.h"
#include "runtime/core/error/error.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace Incubator::FileIO;

namespace fs = std::filesystem;

// ============================================================
// 测试夹具
// ============================================================

class FileIOTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 每个测试用例使用独立的临时目录
        testDir = fs::temp_directory_path() / "IncubatorFileIOTest";
        testDir /= std::to_string(reinterpret_cast<uintptr_t>(this));
        fs::remove_all(testDir);
        fs::create_directories(testDir);

        fileIO = createPlatformFileIO();
        ASSERT_NE(fileIO, nullptr);
    }

    void TearDown() override
    {
        fileIO.reset();
        // 清理临时目录
        std::error_code ec;
        fs::remove_all(testDir, ec);
    }

    std::string testPath(std::string_view name) const
    {
        return (testDir / name).string();
    }

    void createSampleFile(const std::string& path, const std::string& content)
    {
        std::ofstream ofs(path, std::ios::binary);
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    fs::path testDir;
    std::unique_ptr<FileIO> fileIO;
};

// ============================================================
// 基本文件操作
// ============================================================

TEST_F(FileIOTest, CreateAndWriteFile)
{
    std::string path = testPath("write_test.bin");
    const std::string content = "Hello, FileIO!";

    auto handle = fileIO->open(path, OpenMode::Write | OpenMode::Create);
    ASSERT_NE(handle, nullptr);
    EXPECT_TRUE(handle->isValid());

    std::span<const uint8_t> data(
        reinterpret_cast<const uint8_t*>(content.data()),
        content.size()
    );
    size_t written = handle->write(data);
    EXPECT_EQ(written, content.size());

    handle->close();
    EXPECT_FALSE(handle->isValid());

    EXPECT_TRUE(fileIO->exists(path));
}

TEST_F(FileIOTest, ReadFile)
{
    std::string path = testPath("read_test.txt");
    const std::string expected = "FileIO Read Test Content";
    createSampleFile(path, expected);

    auto handle = fileIO->open(path, OpenMode::Read);
    ASSERT_NE(handle, nullptr);
    EXPECT_TRUE(handle->isValid());

    std::vector<uint8_t> buffer(expected.size() + 16, 0);
    size_t bytesRead = handle->read(buffer);
    EXPECT_EQ(bytesRead, expected.size());

    std::string actual(reinterpret_cast<char*>(buffer.data()), bytesRead);
    EXPECT_EQ(actual, expected);
}

TEST_F(FileIOTest, ReadWriteBinary)
{
    std::string path = testPath("binary_test.bin");
    const std::array<uint8_t, 8> data = {0x00, 0xFF, 0xAB, 0xCD, 0x12, 0x34, 0x56, 0x78};

    // Write
    {
        auto handle = fileIO->open(path, OpenMode::Write | OpenMode::Create);
        ASSERT_NE(handle, nullptr);
        size_t written = handle->write(data);
        EXPECT_EQ(written, data.size());
    }

    // Read back
    {
        auto handle = fileIO->open(path, OpenMode::Read);
        ASSERT_NE(handle, nullptr);

        std::array<uint8_t, 8> readBuf = {};
        size_t bytesRead = handle->read(readBuf);
        EXPECT_EQ(bytesRead, data.size());
        EXPECT_EQ(readBuf, data);
    }
}

// ============================================================
// 文件存在性 & 大小
// ============================================================

TEST_F(FileIOTest, FileExists)
{
    std::string path = testPath("exists_test.txt");
    EXPECT_FALSE(fileIO->exists(path));

    createSampleFile(path, "exists");
    EXPECT_TRUE(fileIO->exists(path));
}

TEST_F(FileIOTest, GetFileSize)
{
    std::string path = testPath("size_test.txt");
    EXPECT_EQ(fileIO->getFileSize(path), 0);

    const std::string content = "1234567890";
    createSampleFile(path, content);
    EXPECT_EQ(fileIO->getFileSize(path), content.size());
}

// ============================================================
// Seek 操作
// ============================================================

TEST_F(FileIOTest, SeekBegin)
{
    std::string path = testPath("seek_begin.txt");
    const std::string content = "ABCDEFGHIJ";
    createSampleFile(path, content);

    auto handle = fileIO->open(path, OpenMode::Read);
    ASSERT_NE(handle, nullptr);

    // Seek to position 5
    size_t newPos = handle->seek(5, SeekOrigin::Begin);
    EXPECT_EQ(newPos, 5);

    char buf = 0;
    auto span = std::span(reinterpret_cast<uint8_t*>(&buf), 1);
    handle->read(span);
    EXPECT_EQ(buf, 'F');
}

TEST_F(FileIOTest, SeekCurrent)
{
    std::string path = testPath("seek_current.txt");
    const std::string content = "ABCDEFGHIJ";
    createSampleFile(path, content);

    auto handle = fileIO->open(path, OpenMode::Read);
    ASSERT_NE(handle, nullptr);

    // Seek 3 from current (after initial position 0)
    size_t newPos = handle->seek(3, SeekOrigin::Current);
    EXPECT_EQ(newPos, 3);

    char buf = 0;
    auto span = std::span(reinterpret_cast<uint8_t*>(&buf), 1);
    handle->read(span);
    EXPECT_EQ(buf, 'D');

    // Tell should now be 4
    EXPECT_EQ(handle->tell(), 4);
}

TEST_F(FileIOTest, SeekEnd)
{
    std::string path = testPath("seek_end.txt");
    const std::string content = "ABCDEFGHIJ";
    createSampleFile(path, content);

    auto handle = fileIO->open(path, OpenMode::Read);
    ASSERT_NE(handle, nullptr);

    // Seek -3 from end
    size_t newPos = handle->seek(-3, SeekOrigin::End);
    EXPECT_EQ(newPos, content.size() - 3);

    char buf = 0;
    auto span = std::span(reinterpret_cast<uint8_t*>(&buf), 1);
    handle->read(span);
    EXPECT_EQ(buf, 'H');
}

// ============================================================
// Tell 操作
// ============================================================

TEST_F(FileIOTest, TellDefault)
{
    std::string path = testPath("tell_test.txt");
    const std::string content = "0123456789";
    createSampleFile(path, content);

    auto handle = fileIO->open(path, OpenMode::Read);
    ASSERT_NE(handle, nullptr);

    EXPECT_EQ(handle->tell(), 0);

    handle->seek(5, SeekOrigin::Begin);
    EXPECT_EQ(handle->tell(), 5);
}

// ============================================================
// 文件删除
// ============================================================

TEST_F(FileIOTest, RemoveFile)
{
    std::string path = testPath("remove_test.txt");
    createSampleFile(path, "to be removed");

    EXPECT_TRUE(fileIO->exists(path));
    EXPECT_TRUE(fileIO->remove(path));
    EXPECT_FALSE(fileIO->exists(path));
    // 删除不存在的文件应返回 false
    EXPECT_FALSE(fileIO->remove(path));
}

// ============================================================
// 目录操作
// ============================================================

TEST_F(FileIOTest, CreateAndRemoveDirectory)
{
    std::string dirPath = testPath("new_directory");

    EXPECT_FALSE(fileIO->directoryExists(dirPath));
    EXPECT_TRUE(fileIO->createDirectory(dirPath));
    EXPECT_TRUE(fileIO->directoryExists(dirPath));

    // 重复创建应返回 false
    EXPECT_FALSE(fileIO->createDirectory(dirPath));

    // 删除空目录
    EXPECT_TRUE(fileIO->removeDirectory(dirPath));
    EXPECT_FALSE(fileIO->directoryExists(dirPath));
}

TEST_F(FileIOTest, DirectoryExists)
{
    EXPECT_TRUE(fileIO->directoryExists(testDir.string()));

    std::string nonExistent = testPath("nonexistent_dir");
    EXPECT_FALSE(fileIO->directoryExists(nonExistent));

    // 文件不是目录
    std::string filePath = testPath("file_not_dir.txt");
    createSampleFile(filePath, "hello");
    EXPECT_FALSE(fileIO->directoryExists(filePath));
}

TEST_F(FileIOTest, RemoveNonEmptyDirectoryFails)
{
    std::string dirPath = testPath("nonempty_dir");
    EXPECT_TRUE(fileIO->createDirectory(dirPath));

    // 在目录中创建文件
    std::string fileInDir = dirPath + "/file.txt";
    createSampleFile(fileInDir, "content");

    // RemoveDirectory should fail (directory not empty)
    EXPECT_FALSE(fileIO->removeDirectory(dirPath));
}

// ============================================================
// FileHandle RAII 行为
// ============================================================

TEST_F(FileIOTest, FileHandleAutoCloseOnDestruct)
{
    std::string path = testPath("raii_test.txt");
    createSampleFile(path, "RAII test");

    {
        auto handle = fileIO->open(path, OpenMode::Read);
        ASSERT_NE(handle, nullptr);
        EXPECT_TRUE(handle->isValid());
        // handle destroyed here -> auto close
    }
    // File should still exist (close doesn't delete)
    EXPECT_TRUE(fileIO->exists(path));
}

TEST_F(FileIOTest, MoveFileHandle)
{
    std::string path = testPath("move_test.txt");
    createSampleFile(path, "move semantics");

    auto handle1 = fileIO->open(path, OpenMode::Read);
    ASSERT_NE(handle1, nullptr);

    // Move construct
    auto handle2 = std::move(handle1);
    EXPECT_EQ(handle1, nullptr);  // NOLINT
    EXPECT_NE(handle2, nullptr);
    EXPECT_TRUE(handle2->isValid());
}

// ============================================================
// 错误处理
// ============================================================

TEST_F(FileIOTest, OpenNonExistentFileReturnsNull)
{
    std::string path = testPath("nonexistent.dat");
    auto handle = fileIO->open(path, OpenMode::Read);
    EXPECT_EQ(handle, nullptr);
}

TEST_F(FileIOTest, OperationsOnInvalidHandleThrow)
{
    // 使用移动后置空的 unique_ptr
    std::string path = testPath("invalid_handle.txt");
    createSampleFile(path, "test");

    auto handle = fileIO->open(path, OpenMode::Read);
    ASSERT_NE(handle, nullptr);
    handle->close();
    EXPECT_FALSE(handle->isValid());

    std::array<uint8_t, 16> buf = {};
    EXPECT_THROW(handle->read(buf), Incubator::Error::Exception);
    EXPECT_THROW(handle->write({}), Incubator::Error::Exception);
    EXPECT_THROW(handle->seek(0, SeekOrigin::Begin), Incubator::Error::Exception);
    EXPECT_THROW((void)handle->tell(), Incubator::Error::Exception);
    EXPECT_THROW((void)handle->size(), Incubator::Error::Exception);
}

// ============================================================
// 边界条件
// ============================================================

TEST_F(FileIOTest, ReadEmptyFile)
{
    std::string path = testPath("empty.txt");
    createSampleFile(path, "");

    auto handle = fileIO->open(path, OpenMode::Read);
    ASSERT_NE(handle, nullptr);

    EXPECT_EQ(handle->size(), 0);

    std::array<uint8_t, 16> buf = {};
    size_t bytesRead = handle->read(buf);
    EXPECT_EQ(bytesRead, 0);
}

TEST_F(FileIOTest, WriteAndReadLargeContent)
{
    std::string path = testPath("large_content.bin");

    // 写入 1MB 数据
    constexpr size_t dataSize = 1024 * 1024;
    std::vector<uint8_t> writeData(dataSize);
    for (size_t i = 0; i < dataSize; ++i)
    {
        writeData[i] = static_cast<uint8_t>(i & 0xFF);
    }

    {
        auto handle = fileIO->open(path, OpenMode::Write | OpenMode::Create);
        ASSERT_NE(handle, nullptr);
        size_t written = handle->write(writeData);
        EXPECT_EQ(written, dataSize);
    }

    // 读回验证
    {
        EXPECT_EQ(fileIO->getFileSize(path), dataSize);

        auto handle = fileIO->open(path, OpenMode::Read);
        ASSERT_NE(handle, nullptr);

        std::vector<uint8_t> readData(dataSize);
        size_t totalRead = 0;
        while (totalRead < dataSize)
        {
            auto span = std::span(readData.data() + totalRead, dataSize - totalRead);
            size_t n = handle->read(span);
            if (n == 0)
                break;
            totalRead += n;
        }
        EXPECT_EQ(totalRead, dataSize);
        EXPECT_EQ(readData, writeData);
    }
}

TEST_F(FileIOTest, CreateAndTruncateExistingFile)
{
    std::string path = testPath("truncate_test.txt");
    createSampleFile(path, "This content will be truncated");

    // 以 Truncate 模式打开
    {
        auto handle = fileIO->open(path, OpenMode::Write | OpenMode::Truncate);
        ASSERT_NE(handle, nullptr);
        EXPECT_EQ(handle->size(), 0);
    }

    // 文件应该存在但为空
    EXPECT_TRUE(fileIO->exists(path));
    EXPECT_EQ(fileIO->getFileSize(path), 0);
}

// ============================================================
// 打开模式测试
// ============================================================

TEST_F(FileIOTest, OpenReadWriteExistingFile)
{
    std::string path = testPath("readwrite_test.txt");
    const std::string content = "Original Content";
    createSampleFile(path, content);

    // 以 ReadWrite 模式打开
    auto handle = fileIO->open(path, OpenMode::ReadWrite);
    ASSERT_NE(handle, nullptr);

    // Read existing
    std::array<char, 16> readBuf = {};
    auto readSpan = std::span(reinterpret_cast<uint8_t*>(readBuf.data()), readBuf.size());
    size_t n = handle->read(readSpan);
    EXPECT_EQ(n, content.size());

    // Seek to beginning and overwrite
    handle->seek(0, SeekOrigin::Begin);
    const std::string newContent = "Modified";
    auto writeSpan = std::span(
        reinterpret_cast<const uint8_t*>(newContent.data()),
        newContent.size()
    );
    handle->write(writeSpan);
    handle->close();

    // Verify
    std::ifstream ifs(path);
    std::string result;
    std::getline(ifs, result);
    EXPECT_TRUE(result.starts_with("Modified"));
}

// ============================================================
// 工厂函数
// ============================================================

TEST_F(FileIOTest, CreatePlatformFileIOReturnsValidInstance)
{
    auto io = createPlatformFileIO();
    EXPECT_NE(io, nullptr);
}
