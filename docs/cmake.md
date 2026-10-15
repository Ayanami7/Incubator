# CMake 构建结构

## 目标

描述项目的 CMake 层级结构，帮助理解构建组织方式。本文档只介绍**大致构成与约定**，不逐模块罗列，避免随模块增删频繁更新。

## 全局约定

| 约定 | 说明 |
|------|------|
| CMake 最低版本 | 3.15 |
| C++ 标准 | C++20（`CMAKE_CXX_STANDARD 20`） |
| 构建类型 | 静态库为主（`BUILD_SHARED_LIBS OFF`） |
| 源码目录 | `src/`，二进制输出 | `build/` |
| 禁止源码内构建 | 根 CMakeLists 检测并报错 |

## 目录结构

```
Incubator/
├── CMakeLists.txt      # 入口：全局设置 + 依次添加子目录
├── 3rdparty/
│   └── CMakeLists.txt  # 第三方依赖库（各库独立 CMakeLists 或子目录）
├── src/
│   ├── CMakeLists.txt  # 源码入口（仅 add_subdirectory 各层）
│   └── <layer>/        # 按层组织（core/platform/resource/...）
│       └── CMakeLists.txt  # 每层定义静态库目标
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

### 库目标（src/ 下各层）

- 每层一个静态库目标（如 `IncubatorRuntime`），头文件与源文件按模块分组收集
- 使用 `FOLDER "Engine"` 属性在 IDE 中分组显示
- 依赖关系通过 `add_dependencies` + `target_link_libraries` 声明
- 平台相关配置（如 MSVC 的 `/utf-8` 编码支持）在库目标处集中设置

### 测试目标（test/ 下）

- 为每个被测模块创建独立可执行目标（`<模块>_test.exe`）
- 链接测试框架与对应的运行时库
- 使用 `FOLDER "Tests"` 属性分组

### 示例目标（examples/ 下）

- 独立可执行目标，演示引擎模块的实际用法
- 链接运行时库与所需第三方库

### 运行资源（assets/）

- 源文件到构建目录使用 `add_custom_command(OUTPUT ... DEPENDS ...)` 与 `copy_if_different`。
- 消费资源的可执行目标显式依赖部署目标；仅修改资产也会触发更新，无需重新链接可执行文件。
- 需要编译的资源显式声明输入、生成命令和输出，编译产物与复制资产共同位于 `<build>/assets/`。

## 扩展新模块的约定

新增一个模块时，遵循以下模式即可（**无需修改本文档**）：

1. 在对应层目录创建源码文件
2. 在该层的 `CMakeLists.txt` 中，把新文件的头文件/源文件加入对应分组变量
3. 在 `test/` 下创建 `<模块>_test.cpp` 并接入测试目标
4. 若需要示例，在 `examples/` 下添加或修改示例

## 注意事项

- 依赖方向保持单向：上层库只能依赖下层库，避免循环依赖
- 测试与示例通过库目标消费引擎能力，不重复编译引擎源码
- 引入新第三方库时，在 `3rdparty/CMakeLists.txt` 中按既有模式注册，并处理 `NOT TARGET` 防重复
