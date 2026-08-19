#define TESTING_ENABLED

#include "runtime/core/utils/utf8.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

using namespace Incubator::Utils;

class Utf8Test : public ::testing::Test
{};

// ========== isValid 测试 ==========

TEST_F(Utf8Test, IsValidAscii)
{
    EXPECT_TRUE(Utf8::isValid("plain ascii"));
}

TEST_F(Utf8Test, IsValidChinese)
{
    EXPECT_TRUE(Utf8::isValid("中文路径/纹理.png"));
}

TEST_F(Utf8Test, IsValidEmpty)
{
    EXPECT_TRUE(Utf8::isValid(""));
}

TEST_F(Utf8Test, IsValidMultiByteSequences)
{
    // 2/3/4 字节序列：é（2 字节）、€（3 字节）、😀（4 字节）
    EXPECT_TRUE(Utf8::isValid("\xc3\xa9"));
    EXPECT_TRUE(Utf8::isValid("\xe2\x82\xac"));
    EXPECT_TRUE(Utf8::isValid("\xf0\x9f\x98\x80"));
}

TEST_F(Utf8Test, IsValidRejectsTruncatedSequence)
{
    // 4 字节序列只给出 3 字节 → 非法
    EXPECT_FALSE(Utf8::isValid("\xf0\x9f\x98"));
}

TEST_F(Utf8Test, IsValidRejectsOverlongEncoding)
{
    // 超长编码：0xC0 0x80 表示 NUL → 非法
    EXPECT_FALSE(Utf8::isValid("\xc0\x80"));
}

TEST_F(Utf8Test, IsValidRejectsLoneContinuation)
{
    EXPECT_FALSE(Utf8::isValid("\x80"));
}

TEST_F(Utf8Test, IsValidRejectsSurrogateRange)
{
    // UTF-8 不允许编码 surrogate（0xED 0xA0 0x80 = U+D800）
    EXPECT_FALSE(Utf8::isValid("\xed\xa0\x80"));
}

// ========== toFsPath / toUtf8String 往返 ==========

TEST_F(Utf8Test, FsPathRoundTripChinese)
{
    std::string utf8 = "资源/纹理/角色.png";
    auto path = Utf8::toFsPath(utf8);
    EXPECT_EQ(Utf8::toUtf8String(path), utf8);
}

TEST_F(Utf8Test, FsPathRoundTripMixed)
{
    std::string utf8 = "assets/素材/texture.png";
    auto path = Utf8::toFsPath(utf8);
    EXPECT_EQ(Utf8::toUtf8String(path), utf8);
}

TEST_F(Utf8Test, FsPathRoundTripAscii)
{
    std::string utf8 = "C:/Program Files/App/bin";
    auto path = Utf8::toFsPath(utf8);
    EXPECT_EQ(Utf8::toUtf8String(path), utf8);
}

TEST_F(Utf8Test, ToUtf8StringEmpty)
{
    EXPECT_EQ(Utf8::toUtf8String(std::filesystem::path()), "");
}

TEST_F(Utf8Test, ToUtf8StringChineseBytesAreUtf8)
{
    auto path = Utf8::toFsPath("纹理.png");
    std::string s = Utf8::toUtf8String(path);
    // 纹理 的 UTF-8 字节序列前缀（\xe7\xba\xb9）
    ASSERT_GE(s.size(), 3);
    EXPECT_EQ(static_cast<unsigned char>(s[0]), 0xE7);
    EXPECT_EQ(static_cast<unsigned char>(s[1]), 0xBA);
    EXPECT_EQ(static_cast<unsigned char>(s[2]), 0xB9);
}