#define TESTING_ENABLED

#include "runtime/core/path/path.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

using namespace Incubator::Path;

class PathTest : public ::testing::Test
{};

template <typename Operation>
void expectStringResults(std::initializer_list<std::pair<std::string_view, std::string_view>> cases,
                         Operation operation)
{
    for (const auto& [input, expected] : cases)
    {
        SCOPED_TRACE(input);
        EXPECT_EQ(operation(std::string(input)), std::string(expected));
    }
}

template <typename Predicate>
void expectPredicateResults(std::initializer_list<std::pair<std::string_view, bool>> cases, Predicate predicate)
{
    for (const auto& [input, expected] : cases)
    {
        SCOPED_TRACE(input);
        EXPECT_EQ(predicate(std::string(input)), expected);
    }
}

TEST_F(PathTest, JoinsPaths)
{
    EXPECT_EQ(join("folder", "file.txt"), "folder/file.txt");
    EXPECT_EQ(join("folder/", "file.txt"), "folder/file.txt");
    EXPECT_EQ(join("a", "b", "c"), "a/b/c");
    EXPECT_EQ(join("C:/dir", "sub", "file.txt"), "C:/dir/sub/file.txt");
    EXPECT_EQ(join("", "file.txt"), "file.txt");
    EXPECT_EQ(join("folder", ""), "folder");
    EXPECT_EQ(join("", ""), "");
    EXPECT_EQ(join("中文文件夹", "文件.txt"), "中文文件夹/文件.txt");
    EXPECT_EQ(join("资源/纹理", "角色.png"), "资源/纹理/角色.png");
    EXPECT_EQ(join("assets/素材", "texture.png"), "assets/素材/texture.png");
}

TEST_F(PathTest, NormalizesPaths)
{
    expectStringResults({{"a/b/c", "a/b/c"},
                         {"a/../b", "b"},
                         {"a/./b", "a/b"},
                         {"a//b", "a/b"},
                         {"a\\b/c", "a/b/c"},
                         {"a\\b\\c", "a/b/c"},
                         {"a/b/../c/./d", "a/c/d"},
                         {"", ""},
                         {"C:/Program Files/App/bin", "C:/Program Files/App/bin"},
                         {"C:/a/b/../c", "C:/a/c"},
                         {"//server/share/folder", "//server/share/folder"},
                         {"//server/share/a/../b", "//server/share/b"},
                         {"C:/../other", "C:/other"},
                         {"a/b/", "a/b"},
                         {"中文/./../其他", "其他"},
                         {"游戏\\配置\\设置.ini", "游戏/配置/设置.ini"},
                         {"游戏/./资源/../纹理/./角色.png", "游戏/纹理/角色.png"}},
                        [](const std::string& path) { return normalize(path); });
}

TEST_F(PathTest, ExtractsFileNames)
{
    expectStringResults({{"folder/file.txt", "file.txt"},
                         {"folder/file", "file"},
                         {"file.txt", "file.txt"},
                         {"folder/", ""},
                         {"", ""},
                         {"folder/file.tar.gz", "file.tar.gz"},
                         {"folder\\file.txt", "file.txt"},
                         {"路径/文档.pdf", "文档.pdf"},
                         {"数据/说明", "说明"}},
                        [](const std::string& path) { return getFileName(path); });
}

TEST_F(PathTest, ExtractsStems)
{
    expectStringResults({{"folder/file.txt", "file"},
                         {"folder/file", "file"},
                         {"file.txt", "file"},
                         {"folder/file.tar.gz", "file.tar"},
                         {"", ""},
                         {".hidden", ".hidden"},
                         {".hidden.txt", ".hidden"},
                         {"图片/风景.jpg", "风景"},
                         {"音乐/歌曲", "歌曲"}},
                        [](const std::string& path) { return getStem(path); });
}

TEST_F(PathTest, ExtractsExtensions)
{
    expectStringResults({{"folder/file.txt", "txt"},
                         {"folder/file", ""},
                         {"file.txt", "txt"},
                         {"", ""},
                         {"folder/file.tar.gz", "gz"},
                         {".hidden", ""},
                         {".hidden.txt", "txt"},
                         {"视频/电影.mp4", "mp4"}},
                        [](const std::string& path) { return getExtension(path); });
}

TEST_F(PathTest, ExtractsParentPaths)
{
    expectStringResults({{"folder/file.txt", "folder"},
                         {"a/b/c/file.txt", "a/b/c"},
                         {"folder/", ""},
                         {"file.txt", ""},
                         {"", ""},
                         {"folder\\file.txt", "folder"},
                         {"项目/源码/main.cpp", "项目/源码"}},
                        [](const std::string& path) { return getParentPath(path); });
}

TEST_F(PathTest, DetectsAbsolutePaths)
{
    expectPredicateResults({{"folder/file.txt", false},
                            {"file.txt", false},
                            {"../file.txt", false},
                            {"C:/folder/file.txt", true},
                            {"D:/file.txt", true},
                            {"c:/windows", true},
                            {"", false},
                            {"//server/share/file.txt", true},
                            {"/usr/local/file.txt", false},
                            {"/file.txt", false},
                            {"D:/游戏/存档", true}},
                           [](const std::string& path) { return isAbsolute(path); });
}

TEST_F(PathTest, DetectsRootNames)
{
    expectPredicateResults({{"folder/file.txt", false},
                            {"file.txt", false},
                            {"", false},
                            {"C:/folder/file.txt", true},
                            {"C:foo", true},
                            {"c:/windows", true},
                            {"d:", true},
                            {"//server/share/file.txt", true},
                            {"\\\\server\\share\\file.txt", true},
                            {"/usr/local/file.txt", false},
                            {"/file.txt", false}},
                           [](const std::string& path) { return hasRootName(path); });
}

TEST_F(PathTest, MatchesExtensionsCaseInsensitively)
{
    EXPECT_TRUE(hasExtension("file.txt", "txt"));
    EXPECT_TRUE(hasExtension("file.txt", ".txt"));
    EXPECT_TRUE(hasExtension("file.TXT", "txt"));
    EXPECT_TRUE(hasExtension("file.txt", "TXT"));
    EXPECT_FALSE(hasExtension("file", "txt"));
    EXPECT_FALSE(hasExtension("file.txt", "png"));
    EXPECT_FALSE(hasExtension("", "txt"));
    EXPECT_FALSE(hasExtension("file.txt", ""));
    EXPECT_TRUE(hasExtension("文档.PDF", "pdf"));
}

TEST_F(PathTest, LowercasesASCIIWithoutChangingOtherCharacters)
{
    expectStringResults({{"ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz"},
                         {"already lower", "already lower"},
                         {"中文/纹理", "中文/纹理"},
                         {"Tex123!_@#", "tex123!_@#"},
                         {"", ""},
                         {"Textures/Player.PNG", "textures/player.png"}},
                        [](const std::string& value) { return toLowerASCII(value); });
}
