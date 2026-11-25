#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

namespace Incubator::FileIO
{
    class FileHandle;
}

namespace Incubator::Resource::Detail
{
    /// @brief 从文件起点校验自包含 GLB 2.0 容器及 JSON，文件上限为 256 MiB。
    /// @throws Error::Exception 容器或 JSON 不符合静态单文件模型约定时抛出 IO。
    void checkGlb(FileIO::FileHandle& file);

    /// @brief 将相对于源文件的依赖路径解析为相对于资产根目录的键。
    /// @param directory 源文件所在目录。
    /// @param path 源文件中记录的依赖路径。
    /// @param uriEncoded 路径是否采用 URI 编码。
    /// @return 可用于资源系统的资产根目录相对路径。
    std::string modelDependencyPath(std::string_view directory, std::string_view path, bool uriEncoded);
    /// @brief 解码模型依赖 URI。
    /// @param path URI 编码的依赖路径。
    /// @return 解码后的路径。
    std::string decodeModelUri(std::string_view path);

    /// @brief 检查 Assimp 可能丢弃或超出项目支持范围的源模型信息。
    /// @param path 模型虚拟路径。
    /// @param text 已读取的源文件文本。
    /// @param objDiffuseMaterials 收集 OBJ 中声明漫反射贴图的材质名称。
    void checkModelSource(std::string_view path, std::string_view text,
                          std::unordered_set<std::string>& objDiffuseMaterials);
}  // namespace Incubator::Resource::Detail
