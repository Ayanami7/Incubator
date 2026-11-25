#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Incubator::Resource
{
    class AssetSource;

    /// @brief 指定导入模型的虚拟父目录，空字符串表示资产根。
    struct ModelImportOptions
    {
        std::string targetDirectory = "models";
    };

    /// @brief 返回新建目录和复制文件的规范虚拟路径。
    struct ImportedModelResult
    {
        std::string virtualPath;
        std::string targetDirectory;
        std::vector<std::string> copiedVirtualPaths;
    };

    /// @brief 将外部 glTF、GLB 或 OBJ 及所需依赖复制到资产根下的新目录，同名时添加数字后缀。
    /// @throws Error::Exception 路径、源依赖或复制无效时抛出 IO，并回滚本次创建的项。
    ImportedModelResult importExternalModel(AssetSource& targetSource, const std::filesystem::path& externalSourcePath,
                                            const ModelImportOptions& options = {});
}  // namespace Incubator::Resource
