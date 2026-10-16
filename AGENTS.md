# 项目约束与规范

## 项目概述

Incubator 是一个 C++ 图形渲染器/游戏引擎项目。

## 技术栈

- **语言**: C++ 20
- **构建系统**: CMake 3.15+

## 项目总体结构

```
Incubator/
├── 3rdparty/           # 第三方依赖库
├── assets/             # 资产文件
├── bin/                # 二进制文件输出
├── build/              # 构建输出目录
├── src/                # 源代码目录
├── examples/           # 完整的测试程序
├── test/               # 单元测试
├── .clang-format       # 代码格式化配置
├── .clang-tidy         # 静态分析配置
├── .editorconfig       # 编辑器配置
├── CMakeLists.txt      # CMake 构建配置
└── README.md           # 项目说明文档
```

## 注意事项

无明确要求的前提下不要读写archived目录下的文件   
构建命令需遵循workflow.md中描述的规范

## 快速导航

- [架构设计](./ARCHITECTURE.md) - 设计原则和技术选型
- [CMake结构](./docs/cmake.md) - 项目CMake构建的层级结构
- [开发工作流](./WORKFLOW.md) - 构建、测试、提交规范
- [模块设计](./docs/runtime/) - 各模块的详细设计文档
- [代码风格](./.agents/rules/code_style.md) - 命名规范和格式规则
- [当前任务](./.agents/specs/tasks) - 当前任务列表和分析情况

## 推荐阅读顺序

首次接手时，按以下顺序建立当前项目上下文：

1. 阅读本文件、[开发工作流](./WORKFLOW.md)和[代码风格](./.agents/rules/code_style.md)，明确本机开发约束。
2. 阅读 [ARCHITECTURE.md 的当前阶段](./ARCHITECTURE.md#当前阶段)，了解已实现能力与逻辑分层；涉及构建组织时查阅 [CMake 结构](./docs/cmake.md)。
3. 阅读 [Demo 使用说明](./docs/examples/demo.md)和 [main.cpp](./examples/demo/main.cpp)，串起资产准备、窗口创建、网格上传、UI 与帧提交。
4. 对照 [Render 模块](./docs/runtime/render.md)阅读 [renderer.h](./src/runtime/render/renderer.h)、[frame_input.h](./src/runtime/render/frame_input.h)和 [mesh.h](./src/runtime/render/mesh.h)，掌握应用输入、借用期与 GPU 所有权。
5. 阅读 [Resource 总览](./docs/runtime/resource/resource.md)，按需进入 Manager、Registry 和 Loader 文档，确认资源持有、卸载、路径与 UTF-8 约定。
6. 根据当前任务补读对应实现、测试和模块文档；任务范围见 [.agents/specs/tasks](./.agents/specs/tasks)。

文档与源码存在冲突时，对照当前公开接口及实现确认行为，并同步修正负责该约定的模块文档。


