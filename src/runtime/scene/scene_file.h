#pragma once

#include <string>
#include <string_view>

#include "runtime/scene/scene_description.h"

namespace Incubator::FileIO
{
    class FileIO;
}

namespace Incubator::Scene
{
    /// @brief 编码场景内容为 UTF-8 JSON v1；非法内容抛出 CONFIG。
    std::string encodeScene(const SceneDescription& description);
    /// @brief 解码完整 UTF-8 JSON v1；格式或内容错误抛出带字段信息的 IO。
    SceneDescription decodeScene(std::string_view json);
    /// @brief 从 UTF-8 原生路径同步读取场景文件。
    SceneDescription readScene(FileIO::FileIO& fileIO, std::string_view path);
    /// @brief 先编码再覆盖文件；打开后的失败可能留下截断或部分文件。
    void writeScene(FileIO::FileIO& fileIO, std::string_view path, const SceneDescription& description);
}  // namespace Incubator::Scene
