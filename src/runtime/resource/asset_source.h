#pragma once

#include <filesystem>
#include <string_view>

#include "runtime/platform/fileio/fileio.h"

namespace Incubator::Resource
{
    /// @brief 将相对虚拟文件路径映射到启动时捕获的单个绝对目录根。
    /// @note 借用的 FileIO 须存活至源销毁；消费者须先释放 CPU 持有、清除 Loader 和源绑定。
    /// 仅保证词法边界，不检查文件链接最终指向的位置；资产根是本地受信任目录。
    class AssetSource final
    {
    public:
        /// @brief 捕获目录根；相对根按构造时 cwd 解析，此后不随 cwd 改变。
        explicit AssetSource(FileIO::FileIO& fileIO, const std::filesystem::path& root = "assets");
        AssetSource(const AssetSource&) = delete;
        AssetSource& operator=(const AssetSource&) = delete;

        /// @brief 返回固定的绝对物理根，仅用于文件访问及诊断，不参与资源身份。
        const std::filesystem::path& root() const noexcept
        {
            return root_;
        }
        /// @brief 校验并映射非空的相对 UTF-8 文件路径；绝对、网络及词法越根路径抛 IO。
        std::filesystem::path resolve(std::string_view virtualPath) const;
        /// @brief 查询根内文件，未找到返回 false，其他文件访问错误继续传递。
        bool exists(std::string_view virtualPath) const;
        /// @brief 只读打开根内文件；未找到返回空值，其余错误继续传递。
        std::unique_ptr<FileIO::FileHandle> open(std::string_view virtualPath,
                                                 FileIO::ShareMode share = FileIO::ShareMode::Read) const;
        /// @brief 借用底层文件服务，供接受物理路径的既有读取函数使用。
        FileIO::FileIO& fileIO() const noexcept
        {
            return fileIO_;
        }

    private:
        FileIO::FileIO& fileIO_;
        std::filesystem::path root_;
    };
}  // namespace Incubator::Resource
