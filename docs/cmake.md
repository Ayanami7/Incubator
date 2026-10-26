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

MSVC + Ninja 使用 `/showIncludes` 解析头文件依赖，其前缀按配置时的控制台输出代码页写入规则。
配置与构建需统一使用 UTF-8 代码页，具体命令及旧对象恢复方式见 [WORKFLOW.md](../WORKFLOW.md)。
仅重链测试程序不能修复头文件依赖为空的旧对象。

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

- Core、Platform、Resource、Model、Scene、Render 是逻辑模块，由 `src/runtime/CMakeLists.txt` 按模块收集头文件与源文件，共同编译为 `IncubatorRuntime` 静态库
- 源码分层的依赖方向由模块边界维护；目录层次不代表独立库目标
- 使用 `FOLDER "Engine"` 属性在 IDE 中分组显示
- 依赖关系通过 `add_dependencies` + `target_link_libraries` 声明
- 平台相关配置（如 MSVC 的 `/utf-8` 编码支持）在库目标处集中设置

`scene/transform.h/.cpp`、`scene/camera.h/.cpp`、`scene/entity.h`、`scene/scene.h/.cpp`、`scene/scene_frame.h/.cpp`
及内部共享输入校验头显式收集到 Runtime 的 Scene 文件组；`scene_transform_camera_test`、`scene_test`、
`scene_frame_test` 按既有 test/runtime
源文件发现规则创建，为不依赖窗口或 GPU 的 CPU 测试。真实模型的 Scene 组合验证复用 model_preparation_test
及其现有资源部署、串行 GPU 执行约定，使用正式 SceneFrame 提取和 Renderer 提交接口。

DemoApp 位于 examples/demo，直接组织 Window、FileIO、资源路由、Scene 和 Renderer；Runtime 不提供 Application 模块，也不依赖示例资产部署。

### 测试目标（test/ 下）

- 为每个被测模块创建独立可执行目标（`<模块>_test.exe`）
- 链接测试框架与对应的运行时库
- 使用 `FOLDER "UnitTest/runtime"` 属性分组

正式测试由 CTest 注册和运行，保留 `gtest_discover_tests()` 的逐用例注册及各目标的 `WORKING_DIRECTORY`、`RUN_SERIAL`、`TIMEOUT` 属性。
VS Code 测试面板使用 TestMate 提供程序/套件/用例层级及批量运行，关闭 CMake Tools 的 CTest 面板集成；命令行 CTest 继续用于正式复核和阶段验收，入口分工见 [开发工作流](../WORKFLOW.md#6-运行测试)。

| 入口 | 初始工作目录 | 执行与超时 |
|---|---|---|
| TestMate | `<build>/` | 全局并发为 1，允许同一程序批量运行；600 秒限制整个进程 |
| CTest 普通测试 | `<build>/test/runtime/` | 逐用例独立进程 |
| CTest demo_app_test / model_preparation_test | `<build>/` | `RUN_SERIAL TRUE`，逐用例超时 120 秒 |
| CTest vulkan_review_test | `<build>/test-fixtures/` | `RUN_SERIAL TRUE`，逐用例超时 120 秒 |

TestMate 不自动读取 CTest 属性；批量与逐用例隔离、超时粒度和初始目录存在上述差异，编辑器通过结果不替代正式 CTest 验收。窗口/GPU 测试同一时间只运行一轮。
`VulkanReviewTest` 夹具通过编译时的 `RUNTIME_TEST_ASSET_DIR` 进入独立测试资产根，并在结束时恢复原目录；
因此从构建目录直接运行或通过编辑器运行时，也使用 `<build>/test-fixtures/assets/` 中的精确样本。

### 示例目标（examples/ 下）

- 独立可执行目标，演示引擎模块的实际用法
- 链接运行时库与所需第三方库

唯一示例目标 demo 编译 examples/demo/main.cpp、demo_app.cpp 和模型展示代码，链接 IncubatorRuntime 和 imgui，依赖 demo_shaders、demo_models，运行工作目录为构建目录。demo_app_test 编译同一 demo_app.cpp 并复用 model_scene.h 验证生命周期和实体操作；旧几何辅助代码放在 test/runtime/support/render_demo。

### 正式应用目标（apps/ 下，规划，尚未实施）

计划新增 `apps/CMakeLists.txt` 作为正式应用入口，由根 CMake 在 Runtime 和资产目标定义后添加；`apps/incubator/CMakeLists.txt` 定义独立可执行目标 `incubator`，收集本应用的 `main.cpp`、`application.h/.cpp` 及后续界面代码。当前目录结构和根 CMake 的添加顺序仍以已实现内容为准，尚未创建上述目录或目标。

- 正式应用链接 `IncubatorRuntime` 与实际使用的界面依赖，不链接 `demo` 或编译 `examples/demo` 源码；Demo 继续作为独立示例目标。
- 正式应用显式依赖所需 Shader、模型和场景的资产部署目标，沿用 `<build>/assets/` 与增量部署约定。接入时将共用部署目标组织到 `assets/`，使应用构建无需依赖 Demo 可执行目标。
- 开发运行 cwd 固定为 `${CMAKE_BINARY_DIR}`，由 VS Code、Visual Studio 和应用集成测试的 CTest 启动配置明确指定；手动启动也先进入该目录。cwd 与可执行文件所在目录分别约定，资源按 `<cwd>/assets/` 解析，相对场景文件路径按 cwd 解析，加载场景时不切换 cwd。

应用与示例的职责边界见 [总体架构](../ARCHITECTURE.md#应用与示例)，待开发方向见 [TO_DO.md](../.agents/specs/TO_DO.md)。本节仅记录构建方向，不引入新的 Application 框架。

### 运行资源（assets/）

- 源文件到构建目录使用 `add_custom_command(OUTPUT ... DEPENDS ...)` 与 `copy_if_different`。
- 消费资源的可执行目标显式依赖部署目标；仅修改资产也会触发更新，无需重新链接可执行文件。
- 需要编译的资源显式声明输入、生成命令和输出，编译产物与复制资产共同位于 `<build>/assets/`。
- Demo 资产目标 `demo_models` 位于 `assets/CMakeLists.txt`，只显式复制 Avocado glTF/bin/PNG 与 Utah Teapot OBJ。`demo_shaders` 在 `examples/demo/CMakeLists.txt` 编译生产 Shader；Demo 依赖这两个目标。
- `runtime_test_fixtures` 在 `test/runtime/CMakeLists.txt` 将最小模型和精确 PNG 样本部署到独立的 `<build>/test-fixtures/assets/`；`runtime_test_shaders` 复制相同的生产 SPIR-V 到该测试根。Vulkan review 测试另部署 Demo 默认使用的 Avocado 基色 PNG；其他真实模型集成测试按需读取选定模型，不向 `<build>/assets/` 部署测试输入。

### Assimp 依赖

Runtime 模型消费层位于 `src/runtime/model/`，`prepared_model.h/.cpp` 和 `model_instance.h/.cpp` 显式加入现有 `IncubatorRuntime`。
`model_preparation_test` 依赖 `runtime_test_fixtures`、`runtime_test_shaders`、`demo_models` 和 `demo_shaders`，
在构建目录运行；GPU 用例与既有 Vulkan review 使用相同的串行执行和超时约定。

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
