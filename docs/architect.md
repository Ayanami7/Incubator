# CMake文件结构

## 根目录 CMakeLists.txt
- **作用**: 项目入口配置，定义全局设置和路径变量
- **主要功能**:
  - 设置 CMake 最低版本为 3.15
  - 配置 C++20 标准  
  - 定义项目名称 `Incubator` 和版本 `0.1.0`

## 3rdparty/CMakeLists.txt
- **作用**: 管理所有第三方依赖库
- **主要功能**:
  - 按顺序添加第三方库，确保依赖关系正确
  - 每个库通过 `NOT TARGET` 检查避免重复添加
  - 配置各库的构建选项（关闭不必要的示例、测试、文档）

## src/CMakeLists.txt
- **作用**: 源代码模块入口
- **主要功能**: 添加 `runtime` 子目录

## src/runtime/CMakeLists.txt
- **作用**: 运行时模块构建配置
- **主要功能**:
  - 定义目标名称 `IncubatorRuntime`
  - 收集头文件和源文件（按模块分组：CORE_HEADERS, CORE_SOURCES）
  - 创建静态库目标 `add_library(STATIC)`
  - 设置项目属性：`FOLDER "Engine"`（用于 IDE 分组）
  - MSVC 特定配置：`/utf-8` 编码支持
  - 添加依赖关系：`add_dependencies(spdlog::spdlog, fmt::fmt)`
  - 链接依赖库：`target_link_libraries(PUBLIC)`
  - 设置包含目录：`src/` 和 `3rdparty/glm`

## test/runtime/CMakeLists.txt
- **作用**: 单元测试-模块构建配置
- **主要功能**:
  - 定义测试目标 `IncubatorRuntimeTests`
  - 收集测试源文件，为每个模块创建测试文件（如 `string_utils_test.cpp`）
  - 创建可执行文件目标 `add_executable`
  - 设置项目属性：`FOLDER "Tests"`
  - 链接测试框架和运行时库：`target_link_libraries(PUBLIC)`