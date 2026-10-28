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
├── CMakeLists.txt      # 入口：全局设置、子目录与辅助规则
├── incubator.json      # 默认启动配置，部署到构建目录根部
├── cmake/
│   └── assets.cmake    # 运行资产与默认启动配置部署（文件依赖与增量复制）
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
├── assets/            # 资产源文件
├── test/
│   └── CMakeLists.txt  # 测试入口（按被测层/模块建子目录与可执行目标）
└── examples/
    └── CMakeLists.txt  # 示例程序（可执行目标）
```

## 分层构建模式

### 根 CMakeLists.txt

- 设置全局变量：CMake 版本、C++ 标准、路径变量（`SRC_DIR`、`THIRD_PARTY_DIR` 等）
- 依次添加 `3rdparty` → `src`，包含 `cmake/assets.cmake`，再添加 `test` → `examples`

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
- Runtime 测试使用 `FOLDER "UnitTest/runtime"`，Demo 测试使用 `FOLDER "UnitTest/examples/demo"` 分组

`test/runtime/` 保存 Runtime 模块及跨模块集成测试；`test/examples/demo/` 中的 `demo_test`
编译同一份生产 `demo_app.cpp`，验证 Demo 会话及面板接入。`orbit_camera_test` 单独验证示例轨道相机，
`render_input_test` 验证帧参数与 Vulkan 能力判断；这两个目标不创建窗口或初始化 GPU，也不依赖资产部署。
共享截图、GPU 回读和渲染样本辅助代码位于 `test/support/`，精确 fixture 保留在 `test/runtime/fixtures/`。

正式测试由 CTest 注册和运行，保留 `gtest_discover_tests()` 的逐用例注册及各目标的 `WORKING_DIRECTORY`、`RUN_SERIAL`、`TIMEOUT` 属性。
VS Code 测试面板使用 TestMate 提供程序/套件/用例层级及批量运行，关闭 CMake Tools 的 CTest 面板集成；命令行 CTest 继续用于正式复核和阶段验收，入口分工见 [开发工作流](../WORKFLOW.md#6-运行测试)。

| 入口 | 初始工作目录 | 执行与超时 |
|---|---|---|
| TestMate | `<build>/` | 全局并发为 1，允许同一程序批量运行；600 秒限制整个进程 |
| CTest 普通测试 | `<build>/test/runtime/` | 逐用例独立进程 |
| CTest demo_test / model_preparation_test | `<build>/` | `RUN_SERIAL TRUE`，逐用例超时 120 秒 |
| CTest orbit_camera_test | `<build>/` | CPU 用例，逐用例独立进程 |
| CTest vulkan_review_test / imgui_integration_test | `<build>/test-fixtures/` | `RUN_SERIAL TRUE`，逐用例超时 120 秒 |

TestMate 不自动读取 CTest 属性；批量与逐用例隔离、超时粒度和初始目录存在上述差异，编辑器通过结果不替代正式 CTest 验收。窗口/GPU 测试同一时间只运行一轮。
TestMate 通过 `build/test/{runtime,examples/demo}/*_test.exe` 发现两处测试程序。迁移或删除目标后，
应定点删除旧可执行文件，避免编辑器继续发现失效目标。
Agent 常规验证不操作 Test 面板，检查发现配置并按相同 cwd 直接运行测试程序即可；正式验收使用 CTest。
`VulkanReviewTest` 和 `ImGuiIntegrationTest` 夹具通过编译时的 `RUNTIME_TEST_ASSET_DIR` 进入独立测试资产根，并在结束时恢复原目录；
因此从构建目录直接运行或通过编辑器运行时，也使用 `<build>/test-fixtures/assets/` 中的精确样本。
需要修改资产的 Demo/模型测试在自己的工作副本中运行，结束时恢复启动 cwd；不修改项目源资产或共享 fixture。

### 示例目标（examples/ 下）

- 独立可执行目标，演示引擎模块的实际用法
- 链接运行时库与所需第三方库

唯一示例目标 demo 编译 examples/demo/main.cpp、demo_app.cpp、startup_config.cpp 和模型展示代码，链接 IncubatorRuntime 和 imgui，依赖 demo_shaders、demo_models、demo_scenes、demo_config，运行工作目录为构建目录。demo_test 编译同一 demo_app.cpp、startup_config.cpp 并复用 model_scene.h 验证生命周期、启动场景和实体操作；startup_config_test 独立验证配置解析与 FileIO，几何辅助代码放在 test/support/render_demo。

### 正式应用目标（apps/ 下，规划，尚未实施）

计划新增 `apps/CMakeLists.txt` 作为正式应用入口，由根 CMake 在 Runtime 和资产目标定义后添加；`apps/incubator/CMakeLists.txt` 定义独立可执行目标 `incubator`，收集本应用的 `main.cpp`、`application.h/.cpp` 及后续界面代码。当前目录结构和根 CMake 的添加顺序仍以已实现内容为准，尚未创建上述目录或目标。

- 正式应用链接 `IncubatorRuntime` 与实际使用的界面依赖，不链接 `demo` 或编译 `examples/demo` 源码；Demo 继续作为独立示例目标。
- 正式应用显式依赖所需 Shader、模型和场景的资产部署目标，沿用 `<build>/assets/` 与增量部署约定。接入时将共用部署目标组织到 `cmake/assets.cmake`，使应用构建无需依赖 Demo 可执行目标。
- 开发运行 cwd 固定为 `${CMAKE_BINARY_DIR}`，由 VS Code、Visual Studio 和应用集成测试的 CTest 启动配置明确指定；手动启动也先进入该目录。cwd 与可执行文件所在目录分别约定，资源按 `<cwd>/assets/` 解析，相对场景文件路径按 cwd 解析，加载场景时不切换 cwd。

应用与示例的职责边界见 [总体架构](../ARCHITECTURE.md#应用与示例)，待开发方向见 [TO_DO.md](../.agents/specs/TO_DO.md)。本节仅记录构建方向，不引入新的 Application 框架。

### 运行资源（assets/）

- 源文件到构建目录使用 `add_custom_command(OUTPUT ... DEPENDS ...)` 与 `copy_if_different`。
- 消费资源的可执行目标显式依赖部署目标；仅修改资产也会触发更新，无需重新链接可执行文件。
- 需要编译的资源显式声明输入、生成命令和输出，编译产物与复制资产共同位于 `<build>/assets/`。
- Demo 资产目标 `demo_models` 位于 `cmake/assets.cmake`，显式复制 Avocado glTF/bin/PNG、Utah Teapot OBJ 与正式展示地面 ground/ground.obj、ground.mtl。`demo_scenes` 在同一文件中增量部署 `assets/scenes/shadows.json` 与兼容场景 `assets/scenes/mixed.json`；`demo_shaders` 在 `examples/demo/CMakeLists.txt` 编译 `cube.vert`、`cube.frag` 和阴影深度顶点 `shadow.vert`，输出到构建资产目录；`runtime_test_shaders` 增量复制这三份 SPIR-V 到独立测试资产根。Demo 依赖上述部署目标，Demo 默认配置指向 shadows.json，深度 Shader 在首次有效阴影视口中按需准备；其他消费者继续显式决定是否启用。
- `demo_config` 在 `cmake/assets.cmake` 中将仓库根目录的 `incubator.json` 增量复制到 `<build>/incubator.json`，Demo 显式依赖该目标。配置部署与资产部署均为单向复制，运行时保存不回写源文件；新场景仍需显式加入部署规则。
- `runtime_ui_fonts` 在同一文件中部署 `assets/fonts/NotoSansSC-Regular.otf` 与 `OFL.txt` 到 `<build>/assets/fonts/`，Demo 及独立进程场景恢复程序显式依赖该目标。字体来源与许可见 `assets/fonts/SOURCE.md`，不使用开发机系统字体。
- `runtime_test_fixtures` 将同一生产字体及许可复制到 `<build>/test-fixtures/assets/fonts/`，GPU 和 Demo 隔离工作目录继续使用该字体。`imgui_integration_test` 按 GPU 测试属性串行运行于 `<build>/test-fixtures/`，覆盖受控 DPI 更新、像素密度与原生窗口字体呈现；模拟缩放与实际显示证据分别保存。
- 配置解析复用 Assimp 附带的 RapidJSON 头文件，`demo`、`demo_test` 和 `startup_config_test` 分别设置私有包含目录，不对外暴露解析器类型或增加链接库。
- `runtime_test_fixtures` 在 `test/runtime/CMakeLists.txt` 将最小模型和精确 PNG 样本部署到独立的 `<build>/test-fixtures/assets/`；`runtime_test_shaders` 复制相同的生产 SPIR-V 到该测试根。Vulkan review 使用精确纹理 fixture，真实模型集成测试按需读取选定模型；不向 `<build>/assets/` 部署测试输入。

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

### 本地工作区状态

Demo 的停靠布局保存在运行 cwd 的 `incubator-layout.ini`，由 ImGui 直接管理，不作为资产部署，
不写入 `incubator.json` 或场景 JSON。默认启动窗口为 1920×1080，场景目标大小跟随中央图像区域，
不与启动窗口尺寸绑定。Demo 的集成测试在隔离 cwd 保存布局及用户场景，不覆盖生产运行目录中的布局。


阴影精确测试模型位于 `test/runtime/fixtures/models/shadows/`，通过 `runtime_test_fixtures`
部署到独立测试资产根；接收面和投射面不进入 Demo 产品资产，也不依赖展示场景。
