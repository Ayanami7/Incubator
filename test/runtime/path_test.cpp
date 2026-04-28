#define TESTING_ENABLED

#include "runtime/core/path/path.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace Incubator::Path;

class PathTest : public ::testing::Test
{};

// ========== join 测试 ==========

TEST_F(PathTest, JoinSimplePaths)
{
    std::string result = join("folder", "file.txt");
    EXPECT_EQ(result, "folder/file.txt");
}

TEST_F(PathTest, JoinWithSeparator)
{
    std::string result = join("folder/", "file.txt");
    EXPECT_EQ(result, "folder/file.txt");
}

TEST_F(PathTest, JoinWithMultipleParts)
{
    std::string result = join("a", "b", "c");
    EXPECT_EQ(result, "a/b/c");
}

TEST_F(PathTest, JoinWithAbsoluteFirstPart)
{
    std::string result = join("C:/dir", "sub", "file.txt");
    EXPECT_EQ(result, "C:/dir/sub/file.txt");
}

TEST_F(PathTest, JoinEmptyFirstPart)
{
    std::string result = join("", "file.txt");
    EXPECT_EQ(result, "file.txt");
}

TEST_F(PathTest, JoinEmptySecondPart)
{
    std::string result = join("folder", "");
    EXPECT_EQ(result, "folder");
}

TEST_F(PathTest, JoinBothEmpty)
{
    std::string result = join("", "");
    EXPECT_EQ(result, "");
}

// ========== normalize 测试 ==========

TEST_F(PathTest, NormalizeSimplePath)
{
    std::string result = normalize("a/b/c");
    EXPECT_EQ(result, "a/b/c");
}

TEST_F(PathTest, NormalizeDotDot)
{
    std::string result = normalize("a/../b");
    EXPECT_EQ(result, "b");
}

TEST_F(PathTest, NormalizeDot)
{
    std::string result = normalize("a/./b");
    EXPECT_EQ(result, "a/b");
}

TEST_F(PathTest, NormalizeMultipleSeparators)
{
    std::string result = normalize("a//b");
    EXPECT_EQ(result, "a/b");
}

TEST_F(PathTest, NormalizeMixedSeparators)
{
    std::string result = normalize("a\\b/c");
    EXPECT_EQ(result, "a/b/c");
}

TEST_F(PathTest, NormalizeBackslashes)
{
    std::string result = normalize("a\\b\\c");
    EXPECT_EQ(result, "a/b/c");
}

TEST_F(PathTest, NormalizeComplexPath)
{
    std::string result = normalize("a/b/../c/./d");
    EXPECT_EQ(result, "a/c/d");
}

TEST_F(PathTest, NormalizeEmptyPath)
{
    std::string result = normalize("");
    EXPECT_EQ(result, "");
}

TEST_F(PathTest, NormalizeWindowsDrivePath)
{
    std::string result = normalize("C:/Program Files/App/bin");
    EXPECT_EQ(result, "C:/Program Files/App/bin");
}

TEST_F(PathTest, NormalizeWindowsDrivePathWithDotDot)
{
    std::string result = normalize("C:/a/b/../c");
    EXPECT_EQ(result, "C:/a/c");
}

TEST_F(PathTest, NormalizeUNCPath)
{
    std::string result = normalize("//server/share/folder");
    EXPECT_EQ(result, "//server/share/folder");
}

TEST_F(PathTest, NormalizeUNCPathWithDotDot)
{
    std::string result = normalize("//server/share/a/../b");
    EXPECT_EQ(result, "//server/share/b");
}

TEST_F(PathTest, NormalizeDotDotCannotCrossDriveRoot)
{
    std::string result = normalize("C:/../other");
    EXPECT_EQ(result, "C:/../other");
}

TEST_F(PathTest, NormalizeTrailingSeparator)
{
    std::string result = normalize("a/b/");
    EXPECT_EQ(result, "a/b");
}

// ========== getFileName 测试 ==========

TEST_F(PathTest, GetFileNameSimple)
{
    std::string result = getFileName("folder/file.txt");
    EXPECT_EQ(result, "file.txt");
}

TEST_F(PathTest, GetFileNameNoExtension)
{
    std::string result = getFileName("folder/file");
    EXPECT_EQ(result, "file");
}

TEST_F(PathTest, GetFileNameJustFile)
{
    std::string result = getFileName("file.txt");
    EXPECT_EQ(result, "file.txt");
}

TEST_F(PathTest, GetFileNameTrailingSeparator)
{
    std::string result = getFileName("folder/");
    EXPECT_EQ(result, "");
}

TEST_F(PathTest, GetFileNameEmpty)
{
    std::string result = getFileName("");
    EXPECT_EQ(result, "");
}

TEST_F(PathTest, GetFileNameMultipleExtensions)
{
    std::string result = getFileName("folder/file.tar.gz");
    EXPECT_EQ(result, "file.tar.gz");
}

TEST_F(PathTest, GetFileNameWithBackslash)
{
    std::string result = getFileName("folder\\file.txt");
    EXPECT_EQ(result, "file.txt");
}

// ========== getStem 测试 ==========

TEST_F(PathTest, GetStemSimple)
{
    std::string result = getStem("folder/file.txt");
    EXPECT_EQ(result, "file");
}

TEST_F(PathTest, GetStemNoExtension)
{
    std::string result = getStem("folder/file");
    EXPECT_EQ(result, "file");
}

TEST_F(PathTest, GetStemJustFile)
{
    std::string result = getStem("file.txt");
    EXPECT_EQ(result, "file");
}

TEST_F(PathTest, GetStemMultipleExtensions)
{
    std::string result = getStem("folder/file.tar.gz");
    EXPECT_EQ(result, "file.tar");
}

TEST_F(PathTest, GetStemEmpty)
{
    std::string result = getStem("");
    EXPECT_EQ(result, "");
}

TEST_F(PathTest, GetStemDotFile)
{
    std::string result = getStem(".hidden");
    EXPECT_EQ(result, "");
}

TEST_F(PathTest, GetStemDotFileWithExtension)
{
    std::string result = getStem(".hidden.txt");
    EXPECT_EQ(result, ".hidden");
}

// ========== getExtension 测试 ==========

TEST_F(PathTest, GetExtensionSimple)
{
    std::string result = getExtension("folder/file.txt");
    EXPECT_EQ(result, "txt");
}

TEST_F(PathTest, GetExtensionNoExtension)
{
    std::string result = getExtension("folder/file");
    EXPECT_EQ(result, "");
}

TEST_F(PathTest, GetExtensionJustFile)
{
    std::string result = getExtension("file.txt");
    EXPECT_EQ(result, "txt");
}

TEST_F(PathTest, GetExtensionEmpty)
{
    std::string result = getExtension("");
    EXPECT_EQ(result, "");
}

TEST_F(PathTest, GetExtensionMultipleExtensions)
{
    std::string result = getExtension("folder/file.tar.gz");
    EXPECT_EQ(result, "gz");
}

TEST_F(PathTest, GetExtensionDotFile)
{
    std::string result = getExtension(".hidden");
    EXPECT_EQ(result, "");
}

TEST_F(PathTest, GetExtensionDotFileWithExtension)
{
    std::string result = getExtension(".hidden.txt");
    EXPECT_EQ(result, "txt");
}

// ========== getParentPath 测试 ==========

TEST_F(PathTest, GetParentPathSimple)
{
    std::string result = getParentPath("folder/file.txt");
    EXPECT_EQ(result, "folder");
}

TEST_F(PathTest, GetParentPathNested)
{
    std::string result = getParentPath("a/b/c/file.txt");
    EXPECT_EQ(result, "a/b/c");
}

TEST_F(PathTest, GetParentPathTrailingSeparator)
{
    std::string result = getParentPath("folder/");
    EXPECT_EQ(result, "");
}

TEST_F(PathTest, GetParentPathJustFile)
{
    std::string result = getParentPath("file.txt");
    EXPECT_EQ(result, "");
}

TEST_F(PathTest, GetParentPathEmpty)
{
    std::string result = getParentPath("");
    EXPECT_EQ(result, "");
}

TEST_F(PathTest, GetParentPathWithBackslash)
{
    std::string result = getParentPath("folder\\file.txt");
    EXPECT_EQ(result, "folder");
}

// ========== isAbsolute 测试 ==========

TEST_F(PathTest, IsAbsoluteRelativePath)
{
    EXPECT_FALSE(isAbsolute("folder/file.txt"));
    EXPECT_FALSE(isAbsolute("file.txt"));
    EXPECT_FALSE(isAbsolute("../file.txt"));
}

TEST_F(PathTest, IsAbsoluteWindowsDrive)
{
    EXPECT_TRUE(isAbsolute("C:/folder/file.txt"));
    EXPECT_TRUE(isAbsolute("D:/file.txt"));
    EXPECT_TRUE(isAbsolute("c:/windows"));
}

TEST_F(PathTest, IsAbsoluteEmpty)
{
    EXPECT_FALSE(isAbsolute(""));
}

TEST_F(PathTest, IsAbsoluteUNCPath)
{
    EXPECT_TRUE(isAbsolute("//server/share/file.txt"));
}

TEST_F(PathTest, IsAbsoluteNotUnixPath)
{
    EXPECT_FALSE(isAbsolute("/usr/local/file.txt"));
    EXPECT_FALSE(isAbsolute("/file.txt"));
}

// ========== hasExtension 测试 ==========

TEST_F(PathTest, HasExtensionSimple)
{
    EXPECT_TRUE(hasExtension("file.txt", "txt"));
}

TEST_F(PathTest, HasExtensionWithDot)
{
    EXPECT_TRUE(hasExtension("file.txt", ".txt"));
}

TEST_F(PathTest, HasExtensionCaseInsensitive)
{
    EXPECT_TRUE(hasExtension("file.TXT", "txt"));
    EXPECT_TRUE(hasExtension("file.txt", "TXT"));
}

TEST_F(PathTest, HasExtensionNoExtension)
{
    EXPECT_FALSE(hasExtension("file", "txt"));
}

TEST_F(PathTest, HasExtensionWrongExtension)
{
    EXPECT_FALSE(hasExtension("file.txt", "png"));
}

TEST_F(PathTest, HasExtensionEmptyFile)
{
    EXPECT_FALSE(hasExtension("", "txt"));
}

TEST_F(PathTest, HasExtensionEmptyExt)
{
    EXPECT_FALSE(hasExtension("file.txt", ""));
}

// ========== 组合测试 ==========

TEST_F(PathTest, PathConstructionExample)
{
    std::string assetPath = join("assets", "textures", "player.png");
    EXPECT_EQ(assetPath, "assets/textures/player.png");

    std::string cleanPath = normalize("assets/../textures/./player.png");
    EXPECT_EQ(cleanPath, "textures/player.png");

    std::string fileName = getFileName(assetPath);
    EXPECT_EQ(fileName, "player.png");

    std::string stem = getStem(assetPath);
    EXPECT_EQ(stem, "player");

    std::string ext = getExtension(assetPath);
    EXPECT_EQ(ext, "png");

    EXPECT_TRUE(hasExtension(assetPath, "png"));
}

// ========== 中文路径测试 ==========

TEST_F(PathTest, ChineseJoinSimple)
{
    std::string result = join("中文文件夹", "文件.txt");
    EXPECT_EQ(result, "中文文件夹/文件.txt");
}

TEST_F(PathTest, ChineseJoinNested)
{
    std::string result = join("资源/纹理", "角色.png");
    EXPECT_EQ(result, "资源/纹理/角色.png");
}

TEST_F(PathTest, ChineseNormalizeDotDot)
{
    std::string result = normalize("中文/./../其他");
    EXPECT_EQ(result, "其他");
}

TEST_F(PathTest, ChineseNormalizeBackslashes)
{
    std::string result = normalize("游戏/配置/设置.ini");
    EXPECT_EQ(result, "游戏/配置/设置.ini");
}

TEST_F(PathTest, ChineseGetFileNameSimple)
{
    std::string result = getFileName("路径/文档.pdf");
    EXPECT_EQ(result, "文档.pdf");
}

TEST_F(PathTest, ChineseGetFileNameNoExtension)
{
    std::string result = getFileName("数据/说明");
    EXPECT_EQ(result, "说明");
}

TEST_F(PathTest, ChineseGetStemSimple)
{
    std::string result = getStem("图片/风景.jpg");
    EXPECT_EQ(result, "风景");
}

TEST_F(PathTest, ChineseGetStemNoExtension)
{
    std::string result = getStem("音乐/歌曲");
    EXPECT_EQ(result, "歌曲");
}

TEST_F(PathTest, ChineseGetExtension)
{
    std::string result = getExtension("视频/电影.mp4");
    EXPECT_EQ(result, "mp4");
}

TEST_F(PathTest, ChineseGetParentPath)
{
    std::string result = getParentPath("项目/源码/main.cpp");
    EXPECT_EQ(result, "项目/源码");
}

TEST_F(PathTest, ChineseAbsolutePathWindowsDrive)
{
    EXPECT_TRUE(isAbsolute("D:/游戏/存档"));
}

TEST_F(PathTest, ChineseHasExtensionCaseInsensitive)
{
    EXPECT_TRUE(hasExtension("文档.PDF", "pdf"));
}

TEST_F(PathTest, ChineseMixedWithEnglish)
{
    std::string result = join("assets/素材", "texture.png");
    EXPECT_EQ(result, "assets/素材/texture.png");
}

TEST_F(PathTest, ChineseComplexPath)
{
    std::string path = normalize("游戏/./资源/../纹理/./角色.png");
    EXPECT_EQ(path, "游戏/纹理/角色.png");
}
