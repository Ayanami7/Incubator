#define TESTING_ENABLED

#include "runtime/core/utils/hash.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace Incubator::Utils;

class HashTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        testFile = "test_hash_temp.txt";
        std::ofstream file(testFile);
        file << "Hello, World!" << std::endl;
        file << "This is a test file for hashing." << std::endl;
        file.close();
    }

    void TearDown() override
    {
        if (std::filesystem::exists(testFile))
        {
            std::filesystem::remove(testFile);
        }
    }

    std::filesystem::path testFile;
};

TEST_F(HashTest, HashReturnsConsistentResult)
{
    std::string_view str = "Hello, World!";
    Hash h1 = hash(str);
    Hash h2 = hash(str);

    EXPECT_EQ(h1, h2);
    EXPECT_NE(h1, 0);
}

TEST_F(HashTest, HashDifferentStringsDifferentHashes)
{
    std::string_view str1 = "Hello, World!";
    std::string_view str2 = "Hello, Incubator!";

    Hash h1 = hash(str1);
    Hash h2 = hash(str2);

    EXPECT_NE(h1, h2);
}

TEST_F(HashTest, HashEmptyString)
{
    std::string_view emptyStr = "";
    Hash h = hash(emptyStr);

    EXPECT_NE(h, 0);
}

TEST_F(HashTest, HashLongString)
{
    std::string longStr(10000, 'A');
    Hash h1 = hash(longStr);
    Hash h2 = hash(longStr);

    EXPECT_EQ(h1, h2);
}

TEST_F(HashTest, ConstHashIsCompileTimeConstant)
{
    constexpr Hash h1 = constHash("CompileTime");
    constexpr Hash h2 = constHash("CompileTime");

    static_assert(h1 == h2, "Const hash should be equal");
    static_assert(h1 != 0, "Const hash should not be zero");

    EXPECT_EQ(h1, h2);
}

TEST_F(HashTest, ConstHashDifferentStringsDifferentHashes)
{
    constexpr Hash h1 = constHash("String1");
    constexpr Hash h2 = constHash("String2");

    static_assert(h1 != h2, "Const hashes should be different for different strings");

    EXPECT_NE(h1, h2);
}

TEST_F(HashTest, CombineInteger)
{
    Hash seed = Detail::kOffset;
    combine(seed, static_cast<int32_t>(42));

    EXPECT_NE(seed, Detail::kOffset);
}

TEST_F(HashTest, CombineMultipleValues)
{
    Hash seed1 = Detail::kOffset;
    combine(seed1, static_cast<int32_t>(1));
    combine(seed1, static_cast<int32_t>(2));
    combine(seed1, static_cast<int32_t>(3));

    Hash seed2 = Detail::kOffset;
    combine(seed2, static_cast<int32_t>(1));
    combine(seed2, static_cast<int32_t>(2));

    EXPECT_NE(seed1, seed2);
}

TEST_F(HashTest, CombineFloat)
{
    Hash seed = Detail::kOffset;
    combine(seed, 3.14159f);

    EXPECT_NE(seed, Detail::kOffset);
}

TEST_F(HashTest, CombineDouble)
{
    Hash seed = Detail::kOffset;
    combine(seed, 3.141592653589793);

    EXPECT_NE(seed, Detail::kOffset);
}

TEST_F(HashTest, CombineStringView)
{
    Hash seed = Detail::kOffset;
    combine(seed, std::string_view("TestString"));

    EXPECT_NE(seed, Detail::kOffset);
}

TEST_F(HashTest, CombineStdString)
{
    Hash seed = Detail::kOffset;
    combine(seed, std::string("StdString"));

    EXPECT_NE(seed, Detail::kOffset);
}

TEST_F(HashTest, HashFileFastReturnsValidHash)
{
    if (!std::filesystem::exists(testFile))
    {
        GTEST_SKIP() << "Test file does not exist";
    }

    Hash h1 = hashFileFast(testFile);
    Hash h2 = hashFileFast(testFile);

    EXPECT_EQ(h1, h2);
    EXPECT_NE(h1, 0);
}

TEST_F(HashTest, HashFileFastReturnsZeroForNonExistentFile)
{
    auto nonExistentPath = "non_existent_file_12345.txt";
    Hash h = hashFileFast(nonExistentPath);

    EXPECT_EQ(h, 0);
}

TEST_F(HashTest, HashFileContentReturnsValidHash)
{
    if (!std::filesystem::exists(testFile))
    {
        GTEST_SKIP() << "Test file does not exist";
    }

    Hash h1 = hashFileContent(testFile);
    Hash h2 = hashFileContent(testFile);

    EXPECT_EQ(h1, h2);
    EXPECT_NE(h1, 0);
}

TEST_F(HashTest, HashFileContentThrowsForNonExistentFile)
{
    auto nonExistentPath = "non_existent_file_12345.txt";

    EXPECT_THROW(hashFileContent(nonExistentPath), std::runtime_error);
}

TEST_F(HashTest, HashFileContentDifferentFromFastHash)
{
    if (!std::filesystem::exists(testFile))
    {
        GTEST_SKIP() << "Test file does not exist";
    }

    Hash fastHash = hashFileFast(testFile);
    Hash contentHash = hashFileContent(testFile);

    EXPECT_NE(fastHash, contentHash);
}

TEST_F(HashTest, HashFileContentDetectsFileModification)
{
    if (!std::filesystem::exists(testFile))
    {
        GTEST_SKIP() << "Test file does not exist";
    }

    Hash h1 = hashFileContent(testFile);

    std::ofstream file(testFile, std::ios::app);
    file << "Modified content" << std::endl;
    file.close();

    Hash h2 = hashFileContent(testFile);

    EXPECT_NE(h1, h2);
}

TEST_F(HashTest, CombineOrderMatters)
{
    Hash seed1 = Detail::kOffset;
    combine(seed1, static_cast<int32_t>(1));
    combine(seed1, static_cast<int32_t>(2));

    Hash seed2 = Detail::kOffset;
    combine(seed2, static_cast<int32_t>(2));
    combine(seed2, static_cast<int32_t>(1));

    EXPECT_NE(seed1, seed2);
}

TEST_F(HashTest, HashAndCombineWorkTogether)
{
    Hash seed = Detail::kOffset;
    combine(seed, hash("prefix"));
    combine(seed, static_cast<int32_t>(123));
    combine(seed, hash("suffix"));

    EXPECT_NE(seed, Detail::kOffset);
}
