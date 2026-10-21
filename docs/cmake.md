# CMake 构建结构

## 目标

描述项目的 CMake 层级结构，帮助理解构建组织方式。本文档只介绍**大致构成与约定**，不逐模块罗列，避免随模块增删频繁更新。

## 全局约定

| 约定 | 说明 |
|------|------|
| CMake 最低版本 | 3.22（Assimp v6.0.3 的最低要求） |
| C++ 标准 | C++20（`CMAKE_CXX_STANDARD 20`） |
| 构建类型 | 静态库为主（`BUILD_SHARED_LIBS OFF`） |
| 源码目录 | `src/` |
| 构建输出目录 | `build/` |
| 禁止源码内构建 | 根 CMakeLists 检测并报错 |

## 目录结构

```
Incubator/
├── CMakeLists.txt      # 入口：全局设置 + 依次添加子目录
├── 3rdparty/
│   └── CMakeLists.txt  # 第三方依赖库（各库独立 CMakeLists 或子目录）
├── src/
│   ├── CMakeLists.txt  # 源码入口：add_subdirectory(runtime)
│   └── runtime/
│       ├── CMakeLists.txt  # 收集模块源码，定义唯一的运行时库 IncubatorRuntime
│       ├── core/          # 以下为逻辑模块目录，各自没有 CMakeLists.txt
│       ├── platform/
│       ├── resource/
│       ├── model/
│       ├── scene/
│       ├── application/
│       └── render/
├── assets/
│   └── CMakeLists.txt  # 运行资源部署（文件依赖与增量复制）
├── test/
│   └── CMakeLists.txt  # 测试入口（按被测层/模块建子目录与可执行目标）
└── examples/
    └── CMakeLists.txt  # 示例程序（可执行目标）
```

## 分层构建模式

### 根 CMakeLists.txt

- 设置全局变量：CMake 版本、C++ 标准、路径变量（`SRC_DIR`、`THIRD_PARTY_DIR` 等）
- 依次添加 `3rdparty` → `src` → `assets` → `test` → `examples`

### 运行时库目标（src/runtime/）

- Core、Platform、Resource、Model、Scene、Application、Render 是逻辑模块，由 `src/runtime/CMakeLists.txt` 按模块收集头文件与源文件，共同编译为 `IncubatorRuntime` 静态库
- 源码分层的依赖方向由模块边界维护；目录层次不代表独立库目标
- 使用 `FOLDER "Engine"` 属性在 IDE 中分组显示
- 依赖关系通过 `add_dependencies` + `target_link_libraries` 声明
- 平台相关配置（如 MSVC 的 `/utf-8` 编码支持）在库目标处集中设置

`scene/transform.h/.cpp`、`scene/camera.h/.cpp`、`scene/entity.h`、`scene/scene.h/.cpp`、`scene/scene_frame.h/.cpp`
及内部共享输入校验头显式收集到 Runtime 的 Scene 文件组；`scene_transform_camera_test`、`scene_test`、
`scene_frame_test` 按既有 test/runtime
源文件发现规则创建，为不依赖窗口或 GPU 的 CPU 测试。真实模型的 Scene 组合验证复用 model_preparation_test
及其现有资源部署、串行 GPU 执行约定，使用正式 SceneFrame 提取和 Renderer 提交接口。

`application/application.h/.cpp` 显式归入同一 Runtime，直接组织既有 Window、FileIO、资源路由、Scene 和 Renderer。
`application_test` 依赖 demo_models/demo_textures/demo_shaders，在构建目录串行运行（120 秒超时），
复用既有 GPU/真实窗口支持；Runtime 库本身不依赖具体资产部署目标。

### 测试目标（test/ 下）

- 为每个被测模块创建独立可执行目标（`<模块>_test.exe`）
- 链接测试框架与对应的运行时库
- 使用 `FOLDER "UnitTest/runtime"` 属性分组

### 示例目标（examples/ 下）

- 独立可执行目标，演示引擎模块的实际用法
- 链接运行时库与所需第三方库

### 运行资源（assets/）

- 源文件到构建目录使用 `add_custom_command(OUTPUT ... DEPENDS ...)` 与 `copy_if_different`。
- 消费资源的可执行目标显式依赖部署目标；仅修改资产也会触发更新，无需重新链接可执行文件。
- 需要编译的资源显式声明输入、生成命令和输出，编译产物与复制资产共同位于 `<build>/assets/`。
- 当前 PNG 部署目标 `demo_textures` 定义于 `assets/CMakeLists.txt`，Shader 编译目标 `demo_shaders` 定义于 `examples/demo/CMakeLists.txt`；Demo 和相关 GPU 测试显式依赖这两个目标。
- 模型部署目标 `demo_models` 定义于 `assets/CMakeLists.txt`，按显式文件依赖复制 glTF/bin、OBJ/MTL；Demo、模型准备测试和模型 CPU 导入测试均依赖该目标及 `demo_textures`，前两者还依赖 `demo_shaders`。

### Assimp 依赖

Runtime 模型消费层位于 `src/runtime/model/`，`prepared_model.h/.cpp` 和 `model_instance.h/.cpp` 显式加入现有 `IncubatorRuntime`。
`model_preparation_test` 依赖 `demo_models`、`demo_textures` 和 `demo_shaders`，在构建目录运行；
GPU 用例与既有 Vulkan review 使用相同的串行执行和超时约定。

`3rdparty/assimp/` 内置固定 v6.0.3 源码，由 `3rdparty/CMakeLists.txt` 设置选项并直接 `add_subdirectory(assimp)`，
配置时无需下载 Assimp。移除测试、文档、工具、示例、未启用格式及无关附带库；保留 GLTF/OBJ、完整公开头、
公共代码和后处理，以及上游 CMake 无条件编译的依赖与 STEP/FBX 公共代码。沿用上游 CMake，不按当前调用情况
额外裁剪后处理。当前启用 GLTF/OBJ 导入、使用自带 zlib，关闭导出、Draco、工具、示例、上游测试、文档和安装；
Runtime 私有链接，生成文件及库位于 `build/3rdparty/assimp`。
最低 CMake 为 3.22，MSVC 沿用动态 CRT（Debug `/MDd`、Release `/MD`）；来源、裁剪范围和许可证见
[Assimp 来源说明](../3rdparty/assimp/SOURCE.md)。

## 扩展新模块的约定

新增 Runtime 模块时，沿用当前单库组织：

1. 在 `src/runtime/` 对应逻辑模块目录创建源码文件
2. 在 `src/runtime/CMakeLists.txt` 中，把头文件/源文件加入对应模块分组变量
3. 在 `test/runtime/` 下创建 `<模块>_test.cpp`，现有 `CONFIGURE_DEPENDS` 源文件收集规则会为其创建测试目标
4. 若需要示例，在 `examples/` 下添加或修改示例

## 注意事项

- 逻辑模块的依赖方向保持单向，遵循对应模块文档的职责边界
- 测试与示例通过库目标消费引擎能力，不重复编译引擎源码
- 引入新第三方库时，在 `3rdparty/CMakeLists.txt` 中按既有模式注册，并处理 `NOT TARGET` 防重复
