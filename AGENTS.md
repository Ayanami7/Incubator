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


