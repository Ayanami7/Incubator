#pragma once

#include <string>
#include <string_view>

#include "demo_options.h"
#include "runtime/platform/fileio/fileio.h"

namespace Incubator::Demo
{
    /// @brief 应用启动参数；场景内容由独立的场景文件保存。
    struct StartupConfig
    {
        DemoOptions options;
        std::string startupScene = "assets/scenes/mixed.json";

        StartupConfig();
    };

    /// @brief 解析 UTF-8 启动配置，省略字段使用默认值，未知字段忽略。
    /// @throws Error::Exception 格式或字段无效时抛出 CONFIG。
    StartupConfig decodeStartupConfig(std::string_view json);

    /// @brief 通过现有文件服务读取启动配置；文件不存在时返回默认值。
    /// @note 相对配置路径及其场景路径均按当前工作目录解析，不切换目录。
    /// @throws Error::Exception 读取失败时抛出 IO，配置无效时抛出 CONFIG；消息包含文件路径。
    StartupConfig readStartupConfig(FileIO::FileIO& fileIO, std::string_view path = "incubator.json");
}  // namespace Incubator::Demo
